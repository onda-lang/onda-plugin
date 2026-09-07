#pragma once

#include "Engine.h"
#include "SpscSlot.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace onda::plugin {

struct MidiCapabilities {
  bool noteOn{};
  bool noteOff{};
};

struct WorkerStatus {
  std::uint64_t revision{};
  std::filesystem::path path;
  std::string message{"No Onda file loaded"};
  bool compiling{};
  bool active{};
  bool usingProjectImage{};
  std::uint64_t engineGeneration{};
  std::vector<ParameterMapping> mappings;
  std::vector<BufferMapping> buffers;
  std::vector<EventMapping> events;
  MidiCapabilities midi;
};

struct SeedValues {
  std::uint64_t revision{};
  std::array<float, slotCount> values{};
  std::size_t count{};
};

struct PersistedProjectState {
  std::filesystem::path path;
  std::vector<BufferFileBinding> bufferBindings;
  ProjectImage projectImage;
};

struct EnginePublication {
  WorkerStatus status;
  PersistedProjectState project;
};

struct ProjectExportSnapshot {
  ProjectImage projectImage;
  std::uint64_t generation{};
};

class Worker final {
public:
  enum class ExistingEnginePolicy : std::uint8_t {
    retainUntilSuccess,
    deactivateImmediately,
  };

  using BuildFunction =
      std::function<BuildResult(const std::filesystem::path &, Product, double,
                                int, std::span<const BufferFileBinding>)>;
  using RetirementObserver = void (*)(PreparedEngine *) noexcept;

  Worker(Product product, SpscSlot<PreparedEngine *> &replacements,
         SpscSlot<PreparedEngine *> &retirements,
         std::atomic<bool> &deactivateRequested,
         std::atomic<bool> &replacementSeedPending,
         BuildFunction buildFunction = {},
         RetirementObserver retirementObserver = nullptr);
  ~Worker();

  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;

  void configure(double sampleRate, int blockSize);
  // Host lifecycle/offline callers only; never wait on a realtime thread.
  [[nodiscard]] std::uint64_t waitForPreparation();
  void
  load(std::filesystem::path path, bool seedDefaults,
       ExistingEnginePolicy policy = ExistingEnginePolicy::retainUntilSuccess);
  void loadWithBufferBindings(
      std::filesystem::path path, std::vector<BufferFileBinding> bufferBindings,
      bool seedDefaults,
      ExistingEnginePolicy policy = ExistingEnginePolicy::retainUntilSuccess);
  void restore(std::filesystem::path path, ProjectImage projectImage,
               std::vector<BufferFileBinding> bufferBindings,
               ExistingEnginePolicy policy =
                   ExistingEnginePolicy::deactivateImmediately);
  void unload(bool notifyHost = true);
  void requestRebuild();
  void bindBufferFile(std::string name, std::filesystem::path path);
  void clearBuffer(std::string_view name);
  void setBufferBindings(std::vector<BufferFileBinding> bindings);

  // Called by the audio thread after adoption or retirement; never takes a
  // lock.
  void setActiveGeneration(std::uint64_t generation) noexcept {
    activeGeneration_.store(generation, std::memory_order_release);
  }

  [[nodiscard]] WorkerStatus status() const;
  [[nodiscard]] std::uint64_t statusRevision() const;
  [[nodiscard]] std::optional<SeedValues> takeSeedValues();
  [[nodiscard]] PersistedProjectState persistedProjectState() const;
  [[nodiscard]] std::optional<ProjectExportSnapshot>
  projectExportSnapshot() const;
  [[nodiscard]] bool relinkExport(const ProjectExportSnapshot &snapshot,
                                  std::filesystem::path path);
  [[nodiscard]] bool takeProjectStateChange();
  [[nodiscard]] bool hasPreparedEngine() const noexcept {
    return hasPreparedEngine_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t requestGeneration() const noexcept {
    return requestGeneration_.load(std::memory_order_acquire);
  }

private:
  struct Request {
    std::filesystem::path path;
    double sampleRate{};
    int blockSize{};
    std::uint64_t generation{};
    bool seedDefaults{};
    bool notifyProjectChange{true};
    std::vector<BufferFileBinding> bufferBindings;
    ProjectImage fallbackProjectImage;
  };

  struct FileStamp {
    std::filesystem::path path;
    std::uint64_t hash{};
    std::uintmax_t size{};
    std::filesystem::file_time_type modified{};
    bool exists{};

    friend bool operator==(const FileStamp &, const FileStamp &) = default;
  };

  void run() noexcept;
  void build(const Request &request);
  void advanceGeneration() noexcept;
  // Caller holds mutex_; preserves the current desired buffer bindings.
  void loadLocked(std::filesystem::path path, bool seedDefaults,
                  ExistingEnginePolicy policy);
  void finishRequest(std::uint64_t generation);
  void publishProjectState(PersistedProjectState state, bool notifyHost);
  void deactivateStatus() noexcept;
  void clearPublishedInterface() noexcept;
  void reportFailure(const char *message) noexcept;
  void collectRetired() noexcept;
  void destroy(PreparedEngine *engine) noexcept;
  // Caller holds mutex_. Engine ownership keeps the snapshot alive even while
  // adoption races a newer publication; expired entries are pruned on publish.
  [[nodiscard]] std::shared_ptr<const EnginePublication>
  activePublication() const;
  [[nodiscard]] bool updateStatus(
      const Request &request, std::string message, bool compiling,
      std::vector<BufferMapping> buffers = {});
  [[nodiscard]] bool matchesDesiredLocked(const Request &request) const;
  [[nodiscard]] bool stillCurrent(const Request &request) const;
  [[nodiscard]] static std::vector<FileStamp>
  capture(const std::vector<std::filesystem::path> &paths);
  [[nodiscard]] static std::vector<FileStamp>
  captureMetadata(const std::vector<std::filesystem::path> &paths);
  [[nodiscard]] static bool
  sameMetadata(const std::vector<FileStamp> &left,
               const std::vector<FileStamp> &right) noexcept;
  [[nodiscard]] static bool
  sameContents(const std::vector<FileStamp> &left,
               const std::vector<FileStamp> &right) noexcept;
  [[nodiscard]] static std::vector<std::filesystem::path>
  mergedPaths(std::vector<std::filesystem::path> first,
              const std::vector<std::filesystem::path> &second);

  Product product_;
  SpscSlot<PreparedEngine *> &replacements_;
  SpscSlot<PreparedEngine *> &retirements_;
  std::atomic<bool> &deactivateRequested_;
  std::atomic<bool> &replacementSeedPending_;
  BuildFunction buildFunction_;
  RetirementObserver retirementObserver_{};

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable prepared_;
  Request desired_;
  WorkerStatus status_;
  std::vector<std::weak_ptr<const EnginePublication>> publications_;
  std::optional<SeedValues> seedValues_;
  PersistedProjectState publishedProjectState_;
  bool stopping_{};
  bool forceRebuild_{};
  bool workerStopped_{};
  bool projectStateChanged_{};
  std::atomic<bool> hasPreparedEngine_{};
  std::atomic<std::uint64_t> requestGeneration_{};
  std::atomic<std::uint64_t> activeGeneration_{};

  std::vector<std::filesystem::path> watchedPaths_;
  std::vector<std::filesystem::path> successfulPaths_;
  std::vector<FileStamp> watchedStamp_;
  std::uint64_t completedGeneration_{};
  // Start the thread only after all of its state has been initialized.
  std::thread thread_;
};

} // namespace onda::plugin
