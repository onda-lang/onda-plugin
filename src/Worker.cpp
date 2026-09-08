#include "Worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
               BuildFunction buildFunction,
               const RetirementObserver retirementObserver,
               ParameterSource parameterSource,
               ProjectBuildFunction projectBuildFunction)
    : product_(product), replacements_(replacements), retirements_(retirements),
      deactivateRequested_(deactivateRequested),
      buildFunction_(std::move(buildFunction)),
      projectBuildFunction_(std::move(projectBuildFunction)),
      parameterSource_(std::move(parameterSource)),
      retirementObserver_(retirementObserver), thread_([this] { run(); }) {}

Worker::~Worker() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    advanceGeneration();
  }
  wake_.notify_one();
  prepared_.notify_all();
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
    desired_.recoverCheckpoint = true;
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

std::uint64_t Worker::waitForPreparation() {
  std::unique_lock lock(mutex_);
  prepared_.wait(lock, [this] {
    return stopping_ || workerStopped_ ||
           (desired_.path.empty() && !desired_.fallbackProjectImage.valid()) ||
           !std::isfinite(desired_.sampleRate) || desired_.sampleRate <= 0.0 ||
           desired_.blockSize <= 0 ||
           (!forceRebuild_ && completedGeneration_ == desired_.generation);
  });
  return desired_.generation;
}

void Worker::finishRequest(const std::uint64_t generation) {
  {
    std::lock_guard lock(mutex_);
    completedGeneration_ = generation;
  }
  prepared_.notify_all();
}

// mutex_ is held by every publisher; comparing bytes avoids dirtying the host
// when recompilation produces an identical checkpoint.
void Worker::publishProjectState(PersistedProjectState state,
                                 const bool notifyHost,
                                 std::shared_ptr<SeedValues> pendingDefaults) {
  const auto sameImage =
      publishedProjectState_.projectImage == state.projectImage ||
      (publishedProjectState_.projectImage.valid() &&
       state.projectImage.valid() &&
       *publishedProjectState_.projectImage.bytes == *state.projectImage.bytes);
  const auto changed =
      !sameImage || publishedProjectState_.path != state.path ||
      publishedProjectState_.bufferBindings != state.bufferBindings;
  publishedProjectState_ = std::move(state);
  publishedSeedValues_ = std::move(pendingDefaults);
  projectStateChanged_ = projectStateChanged_ || (notifyHost && changed);
}

bool Worker::takeProjectStateChange() {
  std::lock_guard lock(mutex_);
  return std::exchange(projectStateChanged_, false);
}

void Worker::loadLocked(std::filesystem::path path, const bool seedDefaults,
                        const ExistingEnginePolicy policy) {
  desired_.path = std::move(path);
  desired_.fallbackProjectImage = {};
  desired_.recoverCheckpoint = false;
  desired_.seedDefaults = seedDefaults;
  desired_.notifyProjectChange = true;
  advanceGeneration();
  forceRebuild_ = true;
  status_.path = desired_.path;
  status_.pendingBuffers.reset();
  status_.message = "Waiting to compile";
  status_.compiling = false;
  status_.usingProjectImage = false;
  if (policy == ExistingEnginePolicy::deactivateImmediately) {
    publishProjectState({.path = desired_.path,
                         .bufferBindings = desired_.bufferBindings,
                         .projectImage = {}},
                        true);
    clearPublishedInterface();
    deactivateRequested_.store(true, std::memory_order_release);
  }
  publishIncompleteProjectState();
  ++status_.revision;
}

void Worker::load(std::filesystem::path path, const bool seedDefaults,
                  const ExistingEnginePolicy policy) {
  {
    std::lock_guard lock(mutex_);
    loadLocked(std::move(path), seedDefaults, policy);
  }
  wake_.notify_one();
}

void Worker::loadWithBufferBindings(
    std::filesystem::path path, std::vector<BufferFileBinding> bufferBindings,
    const bool seedDefaults, const ExistingEnginePolicy policy) {
  normalizeBindings(bufferBindings);
  {
    std::lock_guard lock(mutex_);
    desired_.bufferBindings = std::move(bufferBindings);
    loadLocked(std::move(path), seedDefaults, policy);
  }
  wake_.notify_one();
}

std::optional<ProjectExportSnapshot> Worker::projectExportSnapshot() const {
  std::lock_guard lock(mutex_);
  // Do not export an older published project over a pending user selection.
  if (!publishedProjectState_.projectImage.valid() ||
      desired_.path != publishedProjectState_.path ||
      desired_.bufferBindings != publishedProjectState_.bufferBindings ||
      desired_.fallbackProjectImage != publishedProjectState_.projectImage)
    return std::nullopt;
  return ProjectExportSnapshot{publishedProjectState_.projectImage,
                               desired_.generation};
}

bool Worker::relinkExport(const ProjectExportSnapshot &snapshot,
                          std::filesystem::path path) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || snapshot.generation != desired_.generation ||
        snapshot.projectImage != publishedProjectState_.projectImage)
      return false;
    desired_.bufferBindings.clear();
    loadLocked(std::move(path), false,
               ExistingEnginePolicy::retainUntilSuccess);
  }
  wake_.notify_one();
  return true;
}

void Worker::restore(std::filesystem::path path, ProjectImage projectImage,
                     std::vector<BufferFileBinding> bufferBindings,
                     const ExistingEnginePolicy policy) {
  if (path.empty() && !projectImage.valid()) {
    unload(false);
    return;
  }
  normalizeBindings(bufferBindings);
  {
    std::lock_guard lock(mutex_);
    desired_.path = std::move(path);
    desired_.bufferBindings = std::move(bufferBindings);
    desired_.fallbackProjectImage = std::move(projectImage);
    desired_.recoverCheckpoint = false;
    desired_.seedDefaults = false;
    desired_.notifyProjectChange = false;
    projectStateChanged_ = false;
    publishProjectState(
        {
            .path = desired_.path,
            .bufferBindings = desired_.bufferBindings,
            .projectImage = desired_.fallbackProjectImage,
        },
        false);
    advanceGeneration();
    forceRebuild_ = true;
    status_.path = desired_.path;
    status_.pendingBuffers.reset();
    status_.message = "Waiting to restore";
    status_.compiling = false;
    status_.usingProjectImage = false;
    if (policy == ExistingEnginePolicy::deactivateImmediately) {
      clearPublishedInterface();
      deactivateRequested_.store(true, std::memory_order_release);
    }
    ++status_.revision;
  }
  wake_.notify_one();
}

void Worker::unload(const bool notifyHost) {
  {
    std::lock_guard lock(mutex_);
    desired_.path.clear();
    desired_.seedDefaults = false;
    desired_.bufferBindings.clear();
    desired_.fallbackProjectImage = {};
    advanceGeneration();
    forceRebuild_ = false;
    publishProjectState({}, notifyHost);
    if (!notifyHost)
      projectStateChanged_ = false;
    status_.path.clear();
    status_.message = "No Onda file loaded";
    status_.compiling = false;
    clearPublishedInterface();
    status_.usingProjectImage = false;
    deactivateRequested_.store(true, std::memory_order_release);
    ++status_.revision;
  }
  wake_.notify_one();
}

void Worker::requestRebuild() {
  {
    std::lock_guard lock(mutex_);
    desired_.notifyProjectChange = true;
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
    desired_.notifyProjectChange = true;
    advanceGeneration();
    forceRebuild_ = true;
    status_.message = "Waiting to bind audio file";
    status_.compiling = false;
    publishIncompleteProjectState();
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
    desired_.notifyProjectChange = true;
    advanceGeneration();
    forceRebuild_ = true;
    status_.message = "Waiting for buffer binding";
    status_.compiling = false;
    // Editing an incomplete replacement must not discard the running project.
    if (!status_.pendingBuffers) {
      desired_.fallbackProjectImage = {};
      deactivateStatus();
      publishProjectState({.path = desired_.path,
                           .bufferBindings = desired_.bufferBindings,
                           .projectImage = {}},
                          true);
      deactivateRequested_.store(true, std::memory_order_release);
    }
    auto &buffers = status_.pendingBuffers ? *status_.pendingBuffers
                                          : status_.buffers;
    for (auto &buffer : buffers) {
      if (buffer.name == name) {
        buffer.loadedPath.clear();
        buffer.loadedFrames = 0;
        buffer.loadedChannels = 0;
        buffer.loadedSampleRate = 0.0F;
      }
    }
    ++status_.revision;
  }
  wake_.notify_one();
}

void Worker::setBufferBindings(std::vector<BufferFileBinding> bindings) {
  normalizeBindings(bindings);
  {
    std::lock_guard lock(mutex_);
    desired_.bufferBindings = std::move(bindings);
    desired_.fallbackProjectImage = {};
    desired_.notifyProjectChange = true;
    advanceGeneration();
    forceRebuild_ = true;
    publishIncompleteProjectState();
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

std::shared_ptr<SeedValues> Worker::takeSeedValues() {
  std::lock_guard lock(mutex_);
  const auto active = activePublication();
  if (!active || !active->seed || active->seed->claimed)
    return {};
  active->seed->claimed = true;
  return active->seed;
}

void Worker::finishSeeding(const std::shared_ptr<SeedValues> &seed) {
  std::lock_guard lock(mutex_);
  seed->applied.store(true, std::memory_order_release);
  if (seed->revision == desired_.generation)
    desired_.seedDefaults = false;
}

PersistedProjectState Worker::persistedProjectState() const {
  std::lock_guard lock(mutex_);
  return publishedProjectState_;
}

PersistedStateSnapshot Worker::persistedStateSnapshot() const {
  std::lock_guard lock(mutex_);
  return {publishedProjectState_, parameterValuesLocked()};
}

ParameterValues Worker::parameterValuesLocked() const {
  ParameterValues values;
  values.fill(0.5F);
  if (parameterSource_)
    values = parameterSource_();
  if (publishedSeedValues_ &&
      !publishedSeedValues_->applied.load(std::memory_order_acquire)) {
    std::copy_n(publishedSeedValues_->values.begin(),
                publishedSeedValues_->count, values.begin());
  }
  return values;
}

void Worker::advanceGeneration() noexcept {
  ++desired_.generation;
  requestGeneration_.store(desired_.generation, std::memory_order_release);
}

void Worker::publishIncompleteProjectState() {
  // Until a complete checkpoint exists, save the user's source and bindings
  // immediately, even before host preparation or while a buffer is missing.
  if (!publishedProjectState_.projectImage.valid()) {
    publishProjectState({.path = desired_.path,
                         .bufferBindings = desired_.bufferBindings,
                         .projectImage = desired_.fallbackProjectImage},
                        desired_.notifyProjectChange);
  }
}

void Worker::deactivateStatus() noexcept {
  status_.active = false;
  hasPreparedEngine_.store(false, std::memory_order_release);
  status_.engineGeneration = 0;
}

void Worker::clearPublishedInterface() noexcept {
  deactivateStatus();
  status_.mappings.clear();
  status_.buffers.clear();
  status_.pendingBuffers.reset();
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

std::shared_ptr<const EnginePublication> Worker::activePublication() const {
  if (deactivateRequested_.load(std::memory_order_acquire))
    return {};
  const auto generation = activeGeneration_.load(std::memory_order_acquire);
  if (generation != 0) {
    for (const auto &entry : publications_) {
      auto publication = entry.lock();
      if (publication && publication->status.engineGeneration == generation)
        return publication;
    }
  }
  return {};
}

bool Worker::updateStatus(const Request &request, std::string message,
                          const bool compiling,
                          std::vector<BufferMapping> buffers) {
  std::lock_guard lock(mutex_);
  if (!matchesDesiredLocked(request)) {
    return false;
  }
  const auto retained = activePublication();
  if (retained) {
    status_.engineGeneration = retained->status.engineGeneration;
    status_.mappings = retained->status.mappings;
    status_.buffers = retained->status.buffers;
    if (!buffers.empty())
      status_.pendingBuffers = std::move(buffers);
    status_.events = retained->status.events;
    status_.midi = retained->status.midi;
    status_.usingProjectImage = retained->status.usingProjectImage;
    publishProjectState(retained->project, desired_.notifyProjectChange,
                        retained->seed);
    status_.active = true;
    hasPreparedEngine_.store(true, std::memory_order_release);
  } else {
    clearPublishedInterface();
    status_.buffers = std::move(buffers);
  }
  status_.path = desired_.path;
  status_.message = std::move(message);
  status_.compiling = compiling;
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
  PersistedStateSnapshot fallback;
  std::shared_ptr<SeedValues> fallbackSeed;
  {
    std::lock_guard lock(mutex_);
    if (!matchesDesiredLocked(request))
      return;
    hadActiveEngine = activePublication() != nullptr;
    // A failed selection can have no requested image while the last complete
    // checkpoint still owns everything needed for host re-specialization.
    fallback = {publishedProjectState_, parameterValuesLocked()};
    fallbackSeed = publishedSeedValues_;
  }
  if (!updateStatus(request, "Compiling", true))
    return;

  auto prePaths = watchedPaths_;
  if (!request.path.empty())
    prePaths.push_back(request.path);
  for (const auto &binding : request.bufferBindings)
    prePaths.push_back(binding.path);
  prePaths = mergedPaths(std::move(prePaths), {});
  const auto before = capture(prePaths);

  // Use the same host values for disk preparation and image fallback. New
  // patch selections deliberately initialize with their declared defaults.
  std::shared_ptr<SeedValues> inheritedSeed;
  std::optional<ParameterValues> initialParameters;
  if (!request.seedDefaults) {
    std::lock_guard lock(mutex_);
    if (publishedSeedValues_ &&
        !publishedSeedValues_->applied.load(std::memory_order_acquire)) {
      inheritedSeed = publishedSeedValues_;
    }
    if (parameterSource_ || inheritedSeed)
      initialParameters = parameterValuesLocked();
  }
  BuildResult diskResult;
  try {
    std::lock_guard compileLock(compileMutex);
    if (request.path.empty()) {
      diskResult.diagnostic.message = "No linked Onda project path";
    } else {
      diskResult =
          buildFunction_
              ? buildFunction_(request.path, product_, request.sampleRate,
                               request.blockSize, request.bufferBindings,
                               initialParameters)
              : PreparedEngine::build(request.path, product_,
                                      request.sampleRate, request.blockSize,
                                      request.bufferBindings,
                                      initialParameters);
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
  auto after = capture(prePaths);
  const auto knownChanged = before != after;

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
                     false));
    wake_.notify_one();
    return;
  }

  // Keep the validated disk snapshot even if saved-image preparation takes
  // time. Recapturing hereafter would swallow edits made during fallback.
  const auto retainWatchSnapshot = [&after, this] {
    std::erase_if(after, [this](const FileStamp &stamp) {
      return !std::binary_search(watchedPaths_.begin(), watchedPaths_.end(),
                                 stamp.path);
    });
    watchedStamp_ = std::move(after);
  };

  const auto diskFailure = diagnosticMessage(diskResult.diagnostic);
  auto usingProjectImage = false;
  auto attemptedProjectImage = false;
  auto result = std::move(diskResult);
  if (!result.engine && !hadActiveEngine &&
      (request.recoverCheckpoint || request.fallbackProjectImage.valid()) &&
      fallback.project.projectImage.valid() && !buildFunction_) {
    attemptedProjectImage = true;
    static_cast<void>(updateStatus(
        request, "Linked project failed; restoring saved project image", true));
    try {
      std::lock_guard compileLock(compileMutex);
      result = projectBuildFunction_
                   ? projectBuildFunction_(fallback.project.projectImage, product_,
                                           request.sampleRate, request.blockSize,
                                           fallback.parameters)
                   : PreparedEngine::build(fallback.project.projectImage, product_,
                                           request.sampleRate, request.blockSize,
                                           fallback.parameters);
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
    retainWatchSnapshot();
    auto message = diagnosticMessage(result.diagnostic);
    if (attemptedProjectImage)
      message = diskFailure + "; saved project image also failed: " + message;
    static_cast<void>(updateStatus(request, std::move(message), false,
                                   std::move(result.buffers)));
    finishRequest(request.generation);
    return;
  }

  if (!usingProjectImage)
    successfulPaths_ = attemptPaths;
  watchedPaths_ = usingProjectImage
                      ? mergedPaths(successfulPaths_, attemptPaths)
                      : attemptPaths;
  retainWatchSnapshot();

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

    auto seed = usingProjectImage ? fallbackSeed : inheritedSeed;
    if (request.seedDefaults && !usingProjectImage) {
      seed = std::make_shared<SeedValues>();
      seed->revision = request.generation;
      seed->count = std::min(mappings.size(), slotCount);
      for (std::size_t index = 0; index < seed->count; ++index)
        seed->values[index] =
            static_cast<float>(mappings[index].defaultNormalized);
    }

    publishProjectState(
        usingProjectImage
            ? fallback.project
            : PersistedProjectState{.path = desired_.path,
                                    .bufferBindings = desired_.bufferBindings,
                                    .projectImage = result.projectImage},
        desired_.notifyProjectChange, seed);
    desired_.fallbackProjectImage = result.projectImage;
    status_.path = desired_.path;
    status_.message = usingProjectImage
                          ? "Active from saved project: " + diskFailure
                          : "Active";
    status_.compiling = false;
    status_.active = true;
    hasPreparedEngine_.store(true, std::memory_order_release);
    status_.usingProjectImage = usingProjectImage;
    status_.engineGeneration = request.generation;
    status_.mappings = std::move(mappings);
    status_.buffers = std::move(buffers);
    status_.pendingBuffers.reset();
    status_.events = std::move(events);
    status_.midi = midi;
    result.engine->buildGeneration_ = request.generation;
    result.engine->parameterSeed_ = seed;
    result.engine->publication_ = std::make_shared<const EnginePublication>(
        EnginePublication{status_, publishedProjectState_, seed});
    std::erase_if(publications_, [](const auto &entry) { return entry.expired(); });
    publications_.push_back(result.engine->publication_);
    destroy(replacements_.replace(result.engine.release()));

    deactivateRequested_.store(false, std::memory_order_release);
    ++status_.revision;
    completedGeneration_ = request.generation;
  }
  prepared_.notify_all();
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
        wake_.wait_for(lock, pollInterval,
                       [this] { return stopping_ || forceRebuild_; });
        stopped = stopping_;
        request = desired_;
        forceRebuild = std::exchange(forceRebuild_, false);
      }
      if (stopped)
        break;

      if (request.path.empty() && !request.fallbackProjectImage.valid()) {
        destroy(replacements_.tryPop());
        watchedPaths_.clear();
        successfulPaths_.clear();
        watchedStamp_.clear();
        finishRequest(request.generation);
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
        if (matchesDesiredLocked(request)) {
          if (filesChanged && request.generation == completedGeneration_) {
            desired_.notifyProjectChange = true;
            advanceGeneration();
            request = desired_;
          }
          shouldBuild = forceRebuild || filesChanged ||
                        request.generation != completedGeneration_;
        }
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
  {
    std::lock_guard lock(mutex_);
    workerStopped_ = true;
  }
  prepared_.notify_all();
}

} // namespace onda::plugin
