#include "Worker.h"

#include "ProjectPath.h"

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

constexpr auto filesystemFallbackInterval = std::chrono::milliseconds(500);
constexpr auto filesystemDebounceInterval = std::chrono::milliseconds(200);
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
               ProjectBuildFunction projectBuildFunction,
               ParameterMappingsChanged parameterMappingsChanged)
    : product_(product), replacements_(replacements), retirements_(retirements),
      deactivateRequested_(deactivateRequested),
      buildFunction_(std::move(buildFunction)),
      projectBuildFunction_(std::move(projectBuildFunction)),
      parameterSource_(std::move(parameterSource)),
      parameterMappingsChanged_(std::move(parameterMappingsChanged)),
      retirementObserver_(retirementObserver),
      filesystemWatcher_(
          [this](auto paths) { filesystemChanged(std::move(paths)); }),
      thread_([this] { run(); }) {}

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
  static_cast<void>(
      filesystemWatcher_.watch(std::span<const std::filesystem::path>{}));
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
      status_.hasError = false;
      deactivateStatus();
      incrementStatusRevision();
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
                                 std::shared_ptr<SeedValues> pendingDefaults,
                                 std::optional<HostParameterValues> values) {
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
  // Project-only updates retain scalar provenance until a newer host write
  // invalidates its stamp, including across unload and incomplete selections.
  if (values)
    publishedValues_ = std::move(*values);
  hasPendingSeedValues_.store(
      publishedSeedValues_ &&
          !publishedSeedValues_->applied.load(std::memory_order_acquire),
      std::memory_order_release);
  if (notifyHost && changed)
    projectStateChanged_.store(true, std::memory_order_release);
}

bool Worker::takeProjectStateChange() {
  return projectStateChanged_.exchange(false, std::memory_order_acq_rel);
}

void Worker::loadLocked(std::filesystem::path path, const bool seedDefaults,
                        const ExistingEnginePolicy policy) {
  desired_.path = std::move(path);
  desired_.fallbackProjectImage = {};
  desired_.recoverCheckpoint = false;
  desired_.notifyProjectChange = true;
  advanceGeneration();
  desired_.defaultSeedRevision = seedDefaults ? desired_.generation : 0;
  forceRebuild_ = true;
  status_.path = desired_.path;
  status_.pendingBuffers.reset();
  status_.message = "Waiting to compile";
  status_.compiling = false;
  status_.hasError = false;
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
  incrementStatusRevision();
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
                     const ExistingEnginePolicy policy,
                     std::optional<HostParameterValues> values) {
  const auto hasProject = !path.empty() || projectImage.valid();
  if (!hasProject)
    bufferBindings.clear();
  normalizeBindings(bufferBindings);
  {
    std::lock_guard lock(mutex_);
    desired_.path = std::move(path);
    desired_.bufferBindings = std::move(bufferBindings);
    desired_.fallbackProjectImage = std::move(projectImage);
    desired_.recoverCheckpoint = false;
    desired_.defaultSeedRevision = 0;
    desired_.notifyProjectChange = false;
    projectStateChanged_.store(false, std::memory_order_release);
    publishProjectState(
        {
            .path = desired_.path,
            .bufferBindings = desired_.bufferBindings,
            .projectImage = desired_.fallbackProjectImage,
        },
        false, {}, values.value_or(HostParameterValues{}));
    advanceGeneration();
    forceRebuild_ = hasProject;
    status_.path = desired_.path;
    status_.pendingBuffers.reset();
    status_.message = hasProject ? "Waiting to restore" : "No Onda file loaded";
    status_.compiling = false;
    status_.hasError = false;
    status_.usingProjectImage = false;
    if (!hasProject || policy == ExistingEnginePolicy::deactivateImmediately) {
      clearPublishedInterface();
      deactivateRequested_.store(true, std::memory_order_release);
    }
    incrementStatusRevision();
  }
  wake_.notify_one();
}

void Worker::unload(const bool notifyHost) {
  {
    std::lock_guard lock(mutex_);
    desired_.path.clear();
    desired_.defaultSeedRevision = 0;
    desired_.bufferBindings.clear();
    desired_.fallbackProjectImage = {};
    advanceGeneration();
    forceRebuild_ = false;
    publishProjectState({}, notifyHost);
    if (!notifyHost)
      projectStateChanged_.store(false, std::memory_order_release);
    status_.path.clear();
    status_.message = "No Onda file loaded";
    status_.compiling = false;
    status_.hasError = false;
    clearPublishedInterface();
    status_.usingProjectImage = false;
    deactivateRequested_.store(true, std::memory_order_release);
    incrementStatusRevision();
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
    status_.hasError = false;
    publishIncompleteProjectState();
    incrementStatusRevision();
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
    status_.hasError = false;
    auto &buffers =
        status_.pendingBuffers ? *status_.pendingBuffers : status_.buffers;
    const auto buffer = std::ranges::find(buffers, name, &BufferMapping::name);
    // Source-image assets retain file bindings, rather than providing defaults
    // for the linked source when a required binding is cleared.
    const auto hasDefault =
        (desired_.path.empty() || isOndaProjectPath(desired_.path)) &&
        buffer != buffers.end() && buffer->hasProjectDefault;
    // Required bindings invalidate the project immediately. Default-backed and
    // pending replacements retain the complete checkpoint until they succeed.
    if (!status_.pendingBuffers && !hasDefault) {
      desired_.fallbackProjectImage = {};
      deactivateStatus();
      publishProjectState({.path = desired_.path,
                           .bufferBindings = desired_.bufferBindings,
                           .projectImage = {}},
                          true);
      deactivateRequested_.store(true, std::memory_order_release);
    }
    if (buffer != buffers.end() && (status_.pendingBuffers || !hasDefault)) {
      buffer->loadedPath.clear();
      buffer->loadedFrames = 0;
      buffer->loadedChannels = 0;
      buffer->loadedSampleRate = 0.0F;
    }
    incrementStatusRevision();
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
  return publishedStatusRevision_.load(std::memory_order_acquire);
}

void Worker::requestRetirementCollection() {
  {
    std::lock_guard lock(mutex_);
    retirementCollectionRequested_ = true;
  }
  wake_.notify_one();
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
  if (publishedSeedValues_ == seed)
    hasPendingSeedValues_.store(false, std::memory_order_release);
  if (seed->revision == desired_.defaultSeedRevision)
    desired_.defaultSeedRevision = 0;
}

std::size_t Worker::resetParameters(ParameterState &slots,
                                    ControlState &controls,
                                    const ParameterReset bank) {
  std::lock_guard lock(mutex_);
  for (std::size_t index = 0;
       bank != ParameterReset::controls && index < slotCount; ++index) {
    if (index < status_.mappings.size()) {
      const auto value = status_.mappings[index].parameterDefault();
      editorDefaults_.parameters[index] = value;
      editorDefaults_.parameterStamps[index] = slots.setDefault(index, value);
    } else {
      slots.set(index, 0.5F, true);
    }
  }
  for (std::size_t index = 0;
       bank != ParameterReset::parameters && index < status_.controls.size();
       ++index) {
    const auto value = status_.controls[index].controlDefault();
    editorDefaults_.controls[index] = value;
    editorDefaults_.controlStamps[index] = controls.setDefault(index, value);
  }
  return status_.controls.size();
}

PersistedProjectState Worker::persistedProjectState() const {
  std::lock_guard lock(mutex_);
  return publishedProjectState_;
}

PersistedStateSnapshot Worker::persistedStateSnapshot() const {
  std::lock_guard lock(mutex_);
  return {publishedProjectState_, parameterValuesLocked()};
}

HostParameterValues Worker::hostParameterValues() const {
  std::lock_guard lock(mutex_);
  return parameterValuesLocked();
}

HostParameterValues Worker::parameterValuesLocked() const {
  HostParameterValues values;
  values.parameters.fill(0.5F);
  values.controls.fill(0.5F);
  if (parameterSource_)
    values = parameterSource_();
  const auto resolve = [](auto &values, const auto &stamps,
                          const auto &defaults, const auto &defaultStamps,
                          const auto &published, const auto &publishedStamps) {
    for (std::size_t index = 0; index < values.size(); ++index)
      if (stamps[index] != 0U && stamps[index] == defaultStamps[index])
        values[index] = defaults[index];
      else if (stamps[index] != 0U && stamps[index] == publishedStamps[index])
        values[index] = published[index];
  };
  resolve(values.parameters, values.parameterStamps, editorDefaults_.parameters,
          editorDefaults_.parameterStamps, publishedValues_.parameters,
          publishedValues_.parameterStamps);
  resolve(values.controls, values.controlStamps, editorDefaults_.controls,
          editorDefaults_.controlStamps, publishedValues_.controls,
          publishedValues_.controlStamps);
  if (const auto active = activePublication()) {
    for (std::size_t index = 0; index < active->status.controls.size(); ++index)
      if (values.controlStamps[index] != 0U &&
          values.controlStamps[index] ==
              active->initialControlStamps[index].load(
                  std::memory_order_acquire))
        values.controls[index] = active->values.controls[index];
  }
  if (publishedSeedValues_ &&
      !publishedSeedValues_->applied.load(std::memory_order_acquire)) {
    const auto &seed = *publishedSeedValues_;
    const auto started = seed.started.load(std::memory_order_acquire);
    const auto substitute = [started](auto &values, const auto &stamps,
                                      const auto &defaults, const auto count,
                                      const auto &seedStamps) {
      for (std::size_t index = 0; index < count; ++index)
        if (!started || stamps[index] == seedStamps.before[index] ||
            stamps[index] ==
                seedStamps.after[index].load(std::memory_order_acquire))
          values[index] = defaults[index];
    };
    substitute(values.parameters, values.parameterStamps, seed.values,
               seed.count, seed.parameterStamps);
    substitute(values.controls, values.controlStamps, seed.controls,
               seed.controlCount, seed.controlStamps);
  } else if (publishedSeedValues_) {
    const auto &seed = *publishedSeedValues_;
    const auto retain = [](auto &values, const auto &stamps,
                           const auto &defaults, const auto count,
                           const auto &seedStamps) {
      for (std::size_t index = 0; index < count; ++index)
        if (stamps[index] != 0U &&
            stamps[index] ==
                seedStamps.after[index].load(std::memory_order_acquire))
          values[index] = defaults[index];
    };
    retain(values.parameters, values.parameterStamps, seed.values, seed.count,
           seed.parameterStamps);
    retain(values.controls, values.controlStamps, seed.controls,
           seed.controlCount, seed.controlStamps);
  }
  return values;
}

void Worker::advanceGeneration() noexcept {
  ++desired_.generation;
  requestGeneration_.store(desired_.generation, std::memory_order_release);
}

void Worker::incrementStatusRevision() noexcept {
  ++status_.revision;
  publishedStatusRevision_.store(status_.revision, std::memory_order_release);
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

void Worker::setParameterMappings(
    std::vector<ParameterMapping> mappings,
    std::vector<ParameterMapping> controls) noexcept {
  if (status_.mappings == mappings && status_.controls == controls)
    return;
  status_.mappings = std::move(mappings);
  status_.controls = std::move(controls);
  if (parameterMappingsChanged_) {
    try {
      parameterMappingsChanged_();
    } catch (...) {
    }
  }
}

void Worker::clearPublishedInterface() noexcept {
  deactivateStatus();
  setParameterMappings({});
  status_.buffers.clear();
  status_.pendingBuffers.reset();
  status_.events.clear();
  status_.midi = {};
}

void Worker::reportFailure(const char *const message) noexcept {
  try {
    std::lock_guard lock(mutex_);
    status_.compiling = false;
    status_.hasError = true;
    incrementStatusRevision();
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
                          std::vector<BufferMapping> buffers,
                          const bool hasError) {
  std::lock_guard lock(mutex_);
  if (!matchesDesiredLocked(request)) {
    return false;
  }
  const auto retained = activePublication();
  if (retained) {
    status_.engineGeneration = retained->status.engineGeneration;
    setParameterMappings(retained->status.mappings, retained->status.controls);
    status_.buffers = retained->status.buffers;
    if (!buffers.empty())
      status_.pendingBuffers = std::move(buffers);
    status_.events = retained->status.events;
    status_.midi = retained->status.midi;
    status_.usingProjectImage = retained->status.usingProjectImage;
    publishProjectState(retained->project, desired_.notifyProjectChange,
                        retained->seed, retained->values);
    status_.active = true;
    hasPreparedEngine_.store(true, std::memory_order_release);
  } else {
    clearPublishedInterface();
    status_.buffers = std::move(buffers);
  }
  status_.path = desired_.path;
  status_.message = std::move(message);
  status_.compiling = compiling;
  status_.hasError = hasError;
  incrementStatusRevision();
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

void Worker::filesystemChanged(
    std::vector<std::filesystem::path> paths) noexcept {
  try {
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return;
      pendingFilesystemChanges_.insert(pendingFilesystemChanges_.end(),
                                       std::make_move_iterator(paths.begin()),
                                       std::make_move_iterator(paths.end()));
      lastFilesystemChange_ = std::chrono::steady_clock::now();
      ++filesystemChangeSerial_;
    }
    wake_.notify_one();
  } catch (...) {
  }
}

void Worker::refreshFilesystemWatcher() {
  filesystemFallbackPaths_ = filesystemWatcher_.watch(watchedPaths_);
}

bool Worker::watchedFilesChanged(
    const std::vector<std::filesystem::path> &paths,
    const bool allowMetadataShortcut) {
  auto changedPaths = paths;
  std::sort(changedPaths.begin(), changedPaths.end());
  changedPaths.erase(std::unique(changedPaths.begin(), changedPaths.end()),
                     changedPaths.end());

  std::vector<FileStamp> previous;
  std::vector<std::filesystem::path> selected;
  previous.reserve(changedPaths.size());
  selected.reserve(changedPaths.size());
  for (const auto &path : changedPaths) {
    const auto found = std::lower_bound(
        watchedStamp_.begin(), watchedStamp_.end(), path,
        [](const FileStamp &stamp, const std::filesystem::path &candidate) {
          return stamp.path < candidate;
        });
    if (found == watchedStamp_.end() || found->path != path)
      continue;
    previous.push_back(*found);
    selected.push_back(path);
  }
  if (selected.empty())
    return false;

  if (allowMetadataShortcut &&
      sameMetadata(previous, captureMetadata(selected)))
    return false;

  auto current = capture(selected);
  if (!sameContents(previous, current))
    return true;

  for (auto &stamp : current) {
    const auto destination = std::lower_bound(
        watchedStamp_.begin(), watchedStamp_.end(), stamp.path,
        [](const FileStamp &candidate, const std::filesystem::path &path) {
          return candidate.path < path;
        });
    if (destination != watchedStamp_.end() && destination->path == stamp.path) {
      *destination = std::move(stamp);
    }
  }
  return false;
}

void Worker::build(const Request &request) {
  bool hadActiveEngine;
  PersistedStateSnapshot fallback;
  std::shared_ptr<SeedValues> fallbackSeed;
  std::shared_ptr<SeedValues> inheritedSeed;
  std::optional<ParameterValues> initialParameters;
  bool seedDefaults;
  HostParameterValues editorDefaults;
  {
    std::lock_guard lock(mutex_);
    if (!matchesDesiredLocked(request))
      return;
    hadActiveEngine = activePublication() != nullptr;
    // A failed selection can have no requested image while the last complete
    // checkpoint still owns everything needed for host re-specialization.
    fallback = {publishedProjectState_, parameterValuesLocked()};
    fallbackSeed = publishedSeedValues_;
    editorDefaults = editorDefaults_;
    // A selection creates defaults once; its reloads preserve that seed even
    // when host acknowledgment overlaps a newer build generation.
    seedDefaults = request.defaultSeedRevision != 0 &&
                   (!fallbackSeed ||
                    fallbackSeed->revision != request.defaultSeedRevision);
    if (!seedDefaults) {
      if (fallbackSeed &&
          !fallbackSeed->applied.load(std::memory_order_acquire))
        inheritedSeed = fallbackSeed;
      if (parameterSource_ || inheritedSeed)
        initialParameters = fallback.values.parameters;
    }
  }
  if (!updateStatus(request, "Compiling", true))
    return;

  auto prePaths = watchedPaths_;
  if (!request.path.empty())
    prePaths.push_back(request.path);
  for (const auto &binding : request.bufferBindings)
    prePaths.push_back(binding.path);
  prePaths = mergedPaths(std::move(prePaths), {});
  filesystemFallbackPaths_ = filesystemWatcher_.watch(prePaths);
  const auto before = capture(prePaths);
  const auto watchRevision = filesystemWatcher_.revision();

  // Disk preparation and image fallback use the same captured host values.
  // New patch selections deliberately initialize with declared defaults.
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
  // Register newly discovered paths before the post-compile snapshot so no
  // change can fall between validation and native watch installation.
  filesystemFallbackPaths_ =
      filesystemWatcher_.watch(mergedPaths(successfulPaths_, attemptPaths));
  auto after = capture(prePaths);
  const auto knownChanged = before != after;
  const auto changedDuringCompilation =
      filesystemWatcher_.revision() != watchRevision;

  if (!stillCurrent(request))
    return;

  const auto retryChangedSources = [this, &request, &attemptPaths] {
    watchedPaths_ = mergedPaths(successfulPaths_, attemptPaths);
    refreshFilesystemWatcher();
    watchedStamp_ = capture(watchedPaths_);
    {
      std::lock_guard lock(mutex_);
      forceRebuild_ = true;
    }
    static_cast<void>(updateStatus(
        request, "Sources changed during compilation; retrying", false));
    wake_.notify_one();
  };

  if (discoveredNewPath || knownChanged || changedDuringCompilation) {
    retryChangedSources();
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
      result =
          projectBuildFunction_
              ? projectBuildFunction_(fallback.project.projectImage, product_,
                                      request.sampleRate, request.blockSize,
                                      fallback.values.parameters)
              : PreparedEngine::build(fallback.project.projectImage, product_,
                                      request.sampleRate, request.blockSize,
                                      fallback.values.parameters);
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

  // Saved-image preparation may outlive the failed disk attempt. A native
  // event observed in that interval must not be swallowed when watches are
  // reattached below.
  if (filesystemWatcher_.revision() != watchRevision) {
    retryChangedSources();
    return;
  }

  if (!result.engine) {
    watchedPaths_ = mergedPaths(successfulPaths_, attemptPaths);
    retainWatchSnapshot();
    refreshFilesystemWatcher();
    if (watchedFilesChanged(watchedPaths_)) {
      retryChangedSources();
      return;
    }
    auto message = diagnosticMessage(result.diagnostic);
    if (attemptedProjectImage)
      message = diskFailure + "; saved project image also failed: " + message;
    static_cast<void>(updateStatus(request, std::move(message), false,
                                   std::move(result.buffers), true));
    finishRequest(request.generation);
    return;
  }

  if (!usingProjectImage)
    successfulPaths_ = attemptPaths;
  watchedPaths_ = usingProjectImage
                      ? mergedPaths(successfulPaths_, attemptPaths)
                      : attemptPaths;
  retainWatchSnapshot();
  refreshFilesystemWatcher();
  if (watchedFilesChanged(watchedPaths_)) {
    retryChangedSources();
    return;
  }

  std::vector<ParameterMapping> mappings(
      result.engine->parameterMappings().begin(),
      result.engine->parameterMappings().end());
  std::vector<ParameterMapping> controls(
      result.engine->controlMappings().begin(),
      result.engine->controlMappings().end());
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
    if (seedDefaults && !usingProjectImage) {
      seed = std::make_shared<SeedValues>();
      seed->revision = request.defaultSeedRevision;
      seed->count = std::min(mappings.size(), slotCount);
      for (std::size_t index = 0; index < seed->count; ++index)
        seed->values[index] = mappings[index].parameterDefault();
      seed->controlCount = controls.size();
      for (std::size_t index = 0; index < seed->controlCount; ++index)
        seed->controls[index] = controls[index].controlDefault();
    }

    auto checkpointValues = fallback.values;
    // Controls introduced by a reload have no host value until first adoption.
    // Keep inherited selection defaults and every explicit write authoritative.
    for (auto index = seed ? seed->controlCount : std::size_t{0};
         index < controls.size(); ++index)
      if (checkpointValues.controlStamps[index] == ParameterValue::initialStamp)
        checkpointValues.controls[index] = controls[index].controlDefault();
    // A Reset during compilation retains the defaults it actually wrote,
    // even if the replacement declares different defaults with the same
    // normalized float. Later host writes still invalidate these stamps.
    const auto retainResets =
        [](auto &values, auto &stamps, const auto &defaults,
           const auto &defaultStamps, const auto &capturedStamps) {
          for (std::size_t index = 0; index < values.size(); ++index)
            if (defaultStamps[index] != capturedStamps[index]) {
              values[index] = defaults[index];
              stamps[index] = defaultStamps[index];
            }
        };
    retainResets(checkpointValues.parameters, checkpointValues.parameterStamps,
                 editorDefaults_.parameters, editorDefaults_.parameterStamps,
                 editorDefaults.parameterStamps);
    retainResets(checkpointValues.controls, checkpointValues.controlStamps,
                 editorDefaults_.controls, editorDefaults_.controlStamps,
                 editorDefaults.controlStamps);
    publishProjectState(
        usingProjectImage
            ? fallback.project
            : PersistedProjectState{.path = desired_.path,
                                    .bufferBindings = desired_.bufferBindings,
                                    .projectImage = result.projectImage},
        desired_.notifyProjectChange, seed, checkpointValues);
    desired_.fallbackProjectImage = result.projectImage;
    status_.path = desired_.path;
    status_.message = usingProjectImage
                          ? "Active from saved project: " + diskFailure
                          : "Active";
    status_.compiling = false;
    status_.hasError = usingProjectImage && !desired_.path.empty();
    status_.active = true;
    hasPreparedEngine_.store(true, std::memory_order_release);
    status_.usingProjectImage = usingProjectImage;
    status_.engineGeneration = request.generation;
    setParameterMappings(std::move(mappings), std::move(controls));
    status_.buffers = std::move(buffers);
    status_.pendingBuffers.reset();
    status_.events = std::move(events);
    status_.midi = midi;
    result.engine->buildGeneration_ = request.generation;
    result.engine->parameterSeed_ = seed;
    auto publication = std::make_shared<EnginePublication>();
    publication->status = status_;
    publication->project = publishedProjectState_;
    publication->seed = seed;
    publication->values = checkpointValues;
    result.engine->publication_ = std::move(publication);
    result.engine->checkpointValues_ = &result.engine->publication_->values;
    result.engine->initialControlStamps_ =
        &result.engine->publication_->initialControlStamps;
    std::erase_if(publications_,
                  [](const auto &entry) { return entry.expired(); });
    publications_.push_back(result.engine->publication_);
    destroy(replacements_.replace(result.engine.release()));

    deactivateRequested_.store(false, std::memory_order_release);
    incrementStatusRevision();
    completedGeneration_ = request.generation;
  }
  prepared_.notify_all();
}

void Worker::run() noexcept {
  try {
    std::uint64_t observedGeneration{};
    for (;;) {
      collectRetired();

      Request request;
      bool forceRebuild = false;
      bool fallbackValidation = false;
      bool stopped = false;
      std::vector<std::filesystem::path> filesystemChanges;
      {
        std::unique_lock lock(mutex_);
        const auto workRequested = [this, &observedGeneration] {
          return stopping_ || forceRebuild_ || retirementCollectionRequested_ ||
                 !pendingFilesystemChanges_.empty() ||
                 desired_.generation != observedGeneration;
        };
        if (filesystemFallbackPaths_.empty()) {
          wake_.wait(lock, workRequested);
        } else {
          fallbackValidation =
              !wake_.wait_for(lock, filesystemFallbackInterval, workRequested);
        }
        if (!pendingFilesystemChanges_.empty()) {
          auto serial = filesystemChangeSerial_;
          auto deadline = lastFilesystemChange_ + filesystemDebounceInterval;
          for (;;) {
            const auto interrupted = wake_.wait_until(
                lock, deadline, [this, &observedGeneration, &serial] {
                  return stopping_ || forceRebuild_ ||
                         retirementCollectionRequested_ ||
                         desired_.generation != observedGeneration ||
                         filesystemChangeSerial_ != serial;
                });
            if (!interrupted || stopping_ || forceRebuild_ ||
                retirementCollectionRequested_ ||
                desired_.generation != observedGeneration) {
              break;
            }
            serial = filesystemChangeSerial_;
            deadline = lastFilesystemChange_ + filesystemDebounceInterval;
          }
        }
        stopped = stopping_;
        request = desired_;
        observedGeneration = request.generation;
        forceRebuild = std::exchange(forceRebuild_, false);
        retirementCollectionRequested_ = false;
        filesystemChanges = std::move(pendingFilesystemChanges_);
        pendingFilesystemChanges_.clear();
      }
      if (stopped)
        break;

      if (request.path.empty() && !request.fallbackProjectImage.valid()) {
        destroy(replacements_.tryPop());
        watchedPaths_.clear();
        successfulPaths_.clear();
        watchedStamp_.clear();
        refreshFilesystemWatcher();
        finishRequest(request.generation);
        continue;
      }
      if (request.sampleRate <= 0.0 || request.blockSize <= 0)
        continue;

      auto filesChanged = false;
      const auto nativeChange = !filesystemChanges.empty();
      if (nativeChange || fallbackValidation) {
        if (fallbackValidation) {
          filesystemChanges.insert(filesystemChanges.end(),
                                   filesystemFallbackPaths_.begin(),
                                   filesystemFallbackPaths_.end());
        }
        // Reattach macOS inode watches after atomic replacement and move a
        // missing path's watch inward as its directory chain appears.
        refreshFilesystemWatcher();
        // A native invalidation can change bytes without changing metadata.
        // Only fallback polling may skip hashing unchanged metadata.
        filesChanged = watchedFilesChanged(filesystemChanges, !nativeChange);
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
