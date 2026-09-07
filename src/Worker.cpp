#include "Worker.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <new>
#include <system_error>

namespace onda::plugin {
namespace {

constexpr auto pollInterval = std::chrono::milliseconds(200);
// Each VST3 module statically owns one Onda/LLVM image. Serialize compilation
// within that loaded image; separate images do not share compiler state.
std::mutex compileMutex;

std::string diagnosticMessage(const Diagnostic &diagnostic) {
  if (diagnostic.message.empty())
    return "Onda compilation failed";
  if (diagnostic.file.empty() || diagnostic.line <= 0)
    return diagnostic.message;
  return diagnostic.file + ':' + std::to_string(diagnostic.line) + ':' +
         std::to_string(std::max(diagnostic.column, 1)) + ": " +
         diagnostic.message;
}

std::uint64_t hashStream(std::istream &stream) {
  constexpr std::uint64_t offset = 14695981039346656037ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  auto hash = offset;
  std::array<char, 16U * 1024U> bytes{};
  while (stream) {
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto read = stream.gcount();
    for (std::streamsize index = 0; index < read; ++index) {
      hash ^=
          static_cast<unsigned char>(bytes[static_cast<std::size_t>(index)]);
      hash *= prime;
    }
  }
  return hash;
}

void normalizeBindings(std::vector<BufferFileBinding> &bindings) {
  std::erase_if(bindings, [](const BufferFileBinding &binding) {
    return binding.name.empty() || binding.path.empty();
  });
  std::sort(bindings.begin(), bindings.end(),
            [](const BufferFileBinding &left, const BufferFileBinding &right) {
              return left.name < right.name;
            });
  bindings.erase(std::unique(bindings.begin(), bindings.end(),
                             [](const BufferFileBinding &left,
                                const BufferFileBinding &right) {
                               return left.name == right.name;
                             }),
                 bindings.end());
}

} // namespace

Worker::Worker(Product product, SpscSlot<PreparedEngine *> &replacements,
               SpscSlot<PreparedEngine *> &retirements,
               std::atomic<bool> &deactivateRequested,
               std::atomic<bool> &replacementSeedPending,
               BuildFunction buildFunction,
               const RetirementObserver retirementObserver)
    : product_(product), replacements_(replacements), retirements_(retirements),
      deactivateRequested_(deactivateRequested),
      replacementSeedPending_(replacementSeedPending),
      buildFunction_(std::move(buildFunction)),
      retirementObserver_(retirementObserver), thread_([this] { run(); }) {}

Worker::~Worker() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    advanceGeneration();
  }
  wake_.notify_one();
  if (thread_.joinable())
    thread_.join();
  collectRetired();
}

void Worker::configure(const double sampleRate, const int blockSize) {
  {
    std::lock_guard lock(mutex_);
    if (desired_.sampleRate == sampleRate && desired_.blockSize == blockSize)
      return;
    desired_.sampleRate = sampleRate;
    desired_.blockSize = blockSize;
    advanceGeneration();
    forceRebuild_ = true;
    if (!desired_.path.empty() || desired_.fallbackProjectImage.valid()) {
      status_.message = "Waiting for host specialization";
      status_.compiling = false;
      deactivateStatus();
      ++status_.revision;
    }
  }
  wake_.notify_one();
}

void Worker::load(std::filesystem::path path, const bool seedDefaults,
                  const ExistingEnginePolicy policy) {
  {
    std::lock_guard lock(mutex_);
    desired_.path = std::move(path);
    desired_.fallbackProjectImage = {};
    desired_.seedDefaults = seedDefaults;
    advanceGeneration();
    forceRebuild_ = true;
    status_.path = desired_.path;
    status_.message = "Waiting to compile";
    status_.compiling = false;
    status_.usingProjectImage = false;
    if (policy == ExistingEnginePolicy::deactivateImmediately) {
      publishedProjectState_ = {};
      seedValues_.reset();
      clearPublishedInterface();
    }
    ++status_.revision;
  }
  if (policy == ExistingEnginePolicy::deactivateImmediately) {
    deactivateRequested_.store(true, std::memory_order_release);
    replacementSeedPending_.store(false, std::memory_order_release);
  }
  wake_.notify_one();
}

void Worker::loadWithBufferBindings(
    std::filesystem::path path, std::vector<BufferFileBinding> bufferBindings,
    const bool seedDefaults, const ExistingEnginePolicy policy) {
  normalizeBindings(bufferBindings);
  {
    std::lock_guard lock(mutex_);
    desired_.path = std::move(path);
    desired_.bufferBindings = std::move(bufferBindings);
    desired_.fallbackProjectImage = {};
    desired_.seedDefaults = seedDefaults;
    advanceGeneration();
    forceRebuild_ = true;
    status_.path = desired_.path;
    status_.message = "Waiting to compile";
    status_.compiling = false;
    status_.usingProjectImage = false;
    if (policy == ExistingEnginePolicy::deactivateImmediately) {
      publishedProjectState_ = {};
      seedValues_.reset();
      clearPublishedInterface();
    }
    ++status_.revision;
  }
  if (policy == ExistingEnginePolicy::deactivateImmediately) {
    deactivateRequested_.store(true, std::memory_order_release);
    replacementSeedPending_.store(false, std::memory_order_release);
  }
  wake_.notify_one();
}

void Worker::restore(std::filesystem::path path, ProjectImage projectImage,
                     std::vector<BufferFileBinding> bufferBindings,
                     const ExistingEnginePolicy policy) {
  if (!projectImage.valid())
    return;
  normalizeBindings(bufferBindings);
  {
    std::lock_guard lock(mutex_);
    desired_.path = std::move(path);
    desired_.bufferBindings = std::move(bufferBindings);
    desired_.fallbackProjectImage = std::move(projectImage);
    desired_.seedDefaults = false;
    publishedProjectState_ = {
        .path = desired_.path,
        .bufferBindings = desired_.bufferBindings,
        .projectImage = desired_.fallbackProjectImage,
    };
    advanceGeneration();
    forceRebuild_ = true;
    status_.path = desired_.path;
    status_.message = "Waiting to restore";
    status_.compiling = false;
    status_.usingProjectImage = false;
    if (policy == ExistingEnginePolicy::deactivateImmediately) {
      seedValues_.reset();
      clearPublishedInterface();
    }
    ++status_.revision;
  }
  if (policy == ExistingEnginePolicy::deactivateImmediately) {
    deactivateRequested_.store(true, std::memory_order_release);
    replacementSeedPending_.store(false, std::memory_order_release);
  }
  wake_.notify_one();
}

void Worker::unload() {
  {
    std::lock_guard lock(mutex_);
    desired_.path.clear();
    desired_.seedDefaults = false;
    desired_.bufferBindings.clear();
    desired_.fallbackProjectImage = {};
    advanceGeneration();
    forceRebuild_ = false;
    seedValues_.reset();
    publishedProjectState_ = {};
    status_.path.clear();
    status_.message = "No Onda file loaded";
    status_.compiling = false;
    clearPublishedInterface();
    status_.usingProjectImage = false;
    ++status_.revision;
  }
  deactivateRequested_.store(true, std::memory_order_release);
  replacementSeedPending_.store(false, std::memory_order_release);
  wake_.notify_one();
}

void Worker::requestRebuild() {
  {
    std::lock_guard lock(mutex_);
    advanceGeneration();
    forceRebuild_ = true;
  }
  wake_.notify_one();
}

void Worker::bindBufferFile(std::string name, std::filesystem::path path) {
  if (name.empty() || path.empty())
    return;
  {
    std::lock_guard lock(mutex_);
    const auto existing = std::find_if(
        desired_.bufferBindings.begin(), desired_.bufferBindings.end(),
        [&name](const BufferFileBinding &binding) {
          return binding.name == name;
        });
    if (existing != desired_.bufferBindings.end())
      existing->path = std::move(path);
    else
      desired_.bufferBindings.push_back(
          {.name = std::move(name), .path = std::move(path)});
    normalizeBindings(desired_.bufferBindings);
    advanceGeneration();
    forceRebuild_ = true;
    status_.message = "Waiting to bind audio file";
    status_.compiling = false;
    ++status_.revision;
  }
  wake_.notify_one();
}

void Worker::clearBuffer(const std::string_view name) {
  if (name.empty())
    return;
  {
    std::lock_guard lock(mutex_);
    std::erase_if(desired_.bufferBindings,
                  [name](const BufferFileBinding &binding) {
                    return binding.name == name;
                  });
    desired_.fallbackProjectImage = {};
    advanceGeneration();
    forceRebuild_ = true;
    status_.message = "Waiting for buffer binding";
    status_.compiling = false;
    deactivateStatus();
    publishedProjectState_ = {};
    for (auto &buffer : status_.buffers) {
      if (buffer.name == name) {
        buffer.loadedPath.clear();
        buffer.loadedFrames = 0;
        buffer.loadedChannels = 0;
        buffer.loadedSampleRate = 0.0F;
      }
    }
    ++status_.revision;
  }
  deactivateRequested_.store(true, std::memory_order_release);
  replacementSeedPending_.store(false, std::memory_order_release);
  wake_.notify_one();
}

void Worker::setBufferBindings(std::vector<BufferFileBinding> bindings) {
  normalizeBindings(bindings);
  {
    std::lock_guard lock(mutex_);
    desired_.bufferBindings = std::move(bindings);
    desired_.fallbackProjectImage = {};
    advanceGeneration();
    forceRebuild_ = true;
  }
  wake_.notify_one();
}

WorkerStatus Worker::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

std::uint64_t Worker::statusRevision() const {
  std::lock_guard lock(mutex_);
  return status_.revision;
}

std::optional<SeedValues> Worker::takeSeedValues() {
  std::lock_guard lock(mutex_);
  return std::exchange(seedValues_, std::nullopt);
}

PersistedProjectState Worker::persistedProjectState() const {
  std::lock_guard lock(mutex_);
  return publishedProjectState_;
}

void Worker::advanceGeneration() noexcept {
  ++desired_.generation;
  requestGeneration_.store(desired_.generation, std::memory_order_release);
  seedValues_.reset();
  replacementSeedPending_.store(false, std::memory_order_release);
}

void Worker::deactivateStatus() noexcept {
  status_.active = false;
  status_.engineGeneration = 0;
}

void Worker::clearPublishedInterface() noexcept {
  deactivateStatus();
  status_.mappings.clear();
  status_.buffers.clear();
  status_.events.clear();
  status_.midi = {};
}

void Worker::reportFailure(const char *const message) noexcept {
  try {
    std::lock_guard lock(mutex_);
    status_.compiling = false;
    ++status_.revision;
    try {
      status_.message = message;
    } catch (...) {
    }
  } catch (...) {
  }
}

void Worker::destroy(PreparedEngine *const engine) noexcept {
  if (engine != nullptr && retirementObserver_ != nullptr)
    retirementObserver_(engine);
  delete engine;
}

void Worker::collectRetired() noexcept { destroy(retirements_.tryPop()); }

bool Worker::updateStatus(const Request &request, std::string message,
                          const bool compiling, const bool active,
                          std::optional<std::vector<ParameterMapping>> mappings,
                          std::optional<std::vector<BufferMapping>> buffers,
                          std::optional<std::vector<EventMapping>> events) {
  std::lock_guard lock(mutex_);
  if (!matchesDesiredLocked(request)) {
    return false;
  }
  status_.path = desired_.path;
  status_.message = std::move(message);
  status_.compiling = compiling;
  if (active)
    status_.active = true;
  else
    deactivateStatus();
  if (mappings)
    status_.mappings = std::move(*mappings);
  else if (!active)
    status_.mappings.clear();
  if (buffers)
    status_.buffers = std::move(*buffers);
  else if (!active)
    status_.buffers.clear();
  if (events)
    status_.events = std::move(*events);
  else if (!active)
    status_.events.clear();
  if (!active)
    status_.midi = {};
  ++status_.revision;
  return true;
}

bool Worker::matchesDesiredLocked(const Request &request) const {
  return !stopping_ && request.generation == desired_.generation &&
         request.path == desired_.path &&
         request.sampleRate == desired_.sampleRate &&
         request.blockSize == desired_.blockSize &&
         request.bufferBindings == desired_.bufferBindings &&
         request.fallbackProjectImage == desired_.fallbackProjectImage;
}

bool Worker::stillCurrent(const Request &request) const {
  std::lock_guard lock(mutex_);
  return matchesDesiredLocked(request);
}

std::vector<Worker::FileStamp>
Worker::captureMetadata(const std::vector<std::filesystem::path> &paths) {
  std::vector<FileStamp> stamps;
  stamps.reserve(paths.size());
  for (const auto &path : paths) {
    FileStamp stamp{.path = path};
    std::error_code errorCode;
    stamp.exists = std::filesystem::is_regular_file(path, errorCode);
    if (stamp.exists) {
      stamp.size = std::filesystem::file_size(path, errorCode);
      if (!errorCode)
        stamp.modified = std::filesystem::last_write_time(path, errorCode);
      if (errorCode)
        stamp.exists = false;
    }
    stamps.push_back(std::move(stamp));
  }
  return stamps;
}

std::vector<Worker::FileStamp>
Worker::capture(const std::vector<std::filesystem::path> &paths) {
  auto stamps = captureMetadata(paths);
  for (auto &stamp : stamps) {
    if (stamp.exists) {
      std::ifstream stream(stamp.path, std::ios::binary);
      if (stream)
        stamp.hash = hashStream(stream);
      else
        stamp.exists = false;
    }
  }
  return stamps;
}

bool Worker::sameMetadata(const std::vector<FileStamp> &left,
                          const std::vector<FileStamp> &right) noexcept {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const FileStamp &a, const FileStamp &b) {
                      return a.path == b.path && a.size == b.size &&
                             a.modified == b.modified && a.exists == b.exists;
                    });
}

bool Worker::sameContents(const std::vector<FileStamp> &left,
                          const std::vector<FileStamp> &right) noexcept {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const FileStamp &a, const FileStamp &b) {
                      return a.path == b.path && a.hash == b.hash &&
                             a.size == b.size && a.exists == b.exists;
                    });
}

std::vector<std::filesystem::path>
Worker::mergedPaths(std::vector<std::filesystem::path> first,
                    const std::vector<std::filesystem::path> &second) {
  first.insert(first.end(), second.begin(), second.end());
  std::sort(first.begin(), first.end());
  first.erase(std::unique(first.begin(), first.end()), first.end());
  return first;
}

void Worker::build(const Request &request) {
  bool hadActiveEngine;
  {
    std::lock_guard lock(mutex_);
    if (!matchesDesiredLocked(request))
      return;
    const auto active = activeGeneration_.load(std::memory_order_acquire);
    hadActiveEngine =
        active != 0 && !deactivateRequested_.load(std::memory_order_acquire);
    // Publication precedes adoption. Keep the interface of the engine that
    // actually runs, so a superseded queued replacement cannot become fallback.
    if (hadActiveEngine) {
      if (status_.engineGeneration == active)
        retainedStatus_ = status_;
      if (retainedStatus_.engineGeneration == active) {
        status_.engineGeneration = active;
        status_.mappings = retainedStatus_.mappings;
        status_.buffers = retainedStatus_.buffers;
        status_.events = retainedStatus_.events;
        status_.midi = retainedStatus_.midi;
      }
    }
  }
  if (!updateStatus(request, "Compiling", true, hadActiveEngine))
    return;

  auto prePaths = watchedPaths_;
  if (!request.path.empty())
    prePaths.push_back(request.path);
  for (const auto &binding : request.bufferBindings)
    prePaths.push_back(binding.path);
  prePaths = mergedPaths(std::move(prePaths), {});
  const auto before = capture(prePaths);

  BuildResult diskResult;
  try {
    std::lock_guard compileLock(compileMutex);
    if (request.path.empty()) {
      diskResult.diagnostic.message = "No linked Onda project path";
    } else {
      diskResult =
          buildFunction_
              ? buildFunction_(request.path, product_, request.sampleRate,
                               request.blockSize, request.bufferBindings)
              : PreparedEngine::build(request.path, product_,
                                      request.sampleRate, request.blockSize,
                                      request.bufferBindings);
    }
  } catch (const std::bad_alloc &) {
    diskResult.diagnostic.message =
        "Insufficient memory to prepare the Onda specialization";
  } catch (const std::exception &exception) {
    diskResult.diagnostic.message =
        std::string{"Failed to prepare the Onda specialization: "} +
        exception.what();
  }
  auto attemptPaths = std::move(diskResult.watchPaths);
  if (!request.path.empty())
    attemptPaths.push_back(request.path);
  attemptPaths = mergedPaths(std::move(attemptPaths), {});

  const auto discoveredNewPath =
      !std::includes(prePaths.begin(), prePaths.end(), attemptPaths.begin(),
                     attemptPaths.end());
  const auto knownChanged = before != capture(prePaths);

  if (!stillCurrent(request))
    return;

  if (discoveredNewPath || knownChanged) {
    watchedPaths_ = mergedPaths(successfulPaths_, attemptPaths);
    watchedStamp_ = capture(watchedPaths_);
    {
      std::lock_guard lock(mutex_);
      forceRebuild_ = true;
    }
    static_cast<void>(
        updateStatus(request, "Sources changed during compilation; retrying",
                     false, hadActiveEngine));
    wake_.notify_one();
    return;
  }

  const auto diskFailure = diagnosticMessage(diskResult.diagnostic);
  auto usingProjectImage = false;
  auto attemptedProjectImage = false;
  auto result = std::move(diskResult);
  if (!result.engine && !hadActiveEngine &&
      request.fallbackProjectImage.valid() && !buildFunction_) {
    attemptedProjectImage = true;
    static_cast<void>(updateStatus(
        request, "Linked project failed; restoring saved project image", true,
        hadActiveEngine));
    try {
      std::lock_guard compileLock(compileMutex);
      result = PreparedEngine::build(request.fallbackProjectImage, product_,
                                     request.sampleRate, request.blockSize);
      usingProjectImage = result.engine != nullptr;
    } catch (const std::bad_alloc &) {
      result.diagnostic.message =
          "Insufficient memory to restore the saved Onda project image";
    } catch (const std::exception &exception) {
      result.diagnostic.message =
          std::string{"Failed to restore the saved Onda project image: "} +
          exception.what();
    }
  }

  if (!stillCurrent(request))
    return;

  if (!result.engine) {
    watchedPaths_ = mergedPaths(successfulPaths_, attemptPaths);
    watchedStamp_ = capture(watchedPaths_);
    auto message = diagnosticMessage(result.diagnostic);
    if (attemptedProjectImage)
      message = diskFailure + "; saved project image also failed: " + message;
    static_cast<void>(updateStatus(
        request, std::move(message), false, hadActiveEngine, std::nullopt,
        hadActiveEngine ? std::nullopt
                        : std::make_optional(std::move(result.buffers))));
    completedGeneration_ = request.generation;
    return;
  }

  if (!usingProjectImage)
    successfulPaths_ = attemptPaths;
  watchedPaths_ = usingProjectImage
                      ? mergedPaths(successfulPaths_, attemptPaths)
                      : attemptPaths;
  watchedStamp_ = capture(watchedPaths_);

  std::vector<ParameterMapping> mappings(
      result.engine->parameterMappings().begin(),
      result.engine->parameterMappings().end());
  std::vector<BufferMapping> buffers(result.engine->bufferMappings().begin(),
                                     result.engine->bufferMappings().end());
  std::vector<EventMapping> events(result.engine->eventMappings().begin(),
                                   result.engine->eventMappings().end());
  const MidiCapabilities midi{
      .noteOn = result.engine->handlesMidi(MidiKind::noteOn),
      .noteOff = result.engine->handlesMidi(MidiKind::noteOff),
  };
  {
    std::lock_guard lock(mutex_);
    if (!matchesDesiredLocked(request)) {
      return;
    }

    std::optional<SeedValues> seed;
    if (request.seedDefaults) {
      deactivateRequested_.store(true, std::memory_order_release);
      replacementSeedPending_.store(true, std::memory_order_release);
      seed.emplace();
      seed->revision = request.generation;
      seed->count = std::min(mappings.size(), slotCount);
      for (std::size_t index = 0; index < seed->count; ++index)
        seed->values[index] =
            static_cast<float>(mappings[index].defaultNormalized);
    }

    result.engine->buildGeneration_ = request.generation;
    auto *prepared = result.engine.release();
    if (!replacements_.tryPush(prepared)) {
      destroy(prepared);
      replacementSeedPending_.store(false, std::memory_order_release);
      forceRebuild_ = true;
      status_.message = "Realtime replacement handoff is busy; retrying";
      status_.compiling = false;
      ++status_.revision;
      return;
    }

    if (seed) {
      seedValues_ = std::move(seed);
      desired_.seedDefaults = false;
    } else {
      deactivateRequested_.store(false, std::memory_order_release);
    }
    publishedProjectState_ = {
        .path = desired_.path,
        .bufferBindings = desired_.bufferBindings,
        .projectImage = result.projectImage,
    };
    desired_.fallbackProjectImage = result.projectImage;
    status_.path = desired_.path;
    status_.message = usingProjectImage
                          ? "Active from saved project: " + diskFailure
                          : "Active";
    status_.compiling = false;
    status_.active = true;
    status_.usingProjectImage = usingProjectImage;
    status_.engineGeneration = request.generation;
    status_.mappings = std::move(mappings);
    status_.buffers = std::move(buffers);
    status_.events = std::move(events);
    status_.midi = midi;
    ++status_.revision;
    completedGeneration_ = request.generation;
  }
}

void Worker::run() noexcept {
  try {
    for (;;) {
      collectRetired();

      Request request;
      bool forceRebuild = false;
      bool stopped = false;
      {
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, pollInterval);
        stopped = stopping_;
        request = desired_;
        forceRebuild = std::exchange(forceRebuild_, false);
      }
      if (stopped)
        break;

      if (request.path.empty() && !request.fallbackProjectImage.valid()) {
        watchedPaths_.clear();
        successfulPaths_.clear();
        watchedStamp_.clear();
        completedGeneration_ = request.generation;
        continue;
      }
      if (request.sampleRate <= 0.0 || request.blockSize <= 0)
        continue;

      auto filesChanged = false;
      if (!watchedPaths_.empty()) {
        const auto metadata = captureMetadata(watchedPaths_);
        if (!sameMetadata(metadata, watchedStamp_)) {
          auto currentStamp = capture(watchedPaths_);
          filesChanged = !sameContents(currentStamp, watchedStamp_);
          if (!filesChanged)
            watchedStamp_ = std::move(currentStamp);
        }
      }
      bool shouldBuild = false;
      {
        std::lock_guard lock(mutex_);
        shouldBuild = matchesDesiredLocked(request) &&
                      (forceRebuild || filesChanged ||
                       request.generation != completedGeneration_);
      }

      if (shouldBuild)
        build(request);
    }
  } catch (const std::bad_alloc &) {
    reportFailure("Onda worker stopped: insufficient memory");
  } catch (const std::exception &) {
    reportFailure("Onda worker stopped after an unexpected failure");
  } catch (...) {
    reportFailure("Onda worker stopped after an unknown failure");
  }
  collectRetired();
}

} // namespace onda::plugin
