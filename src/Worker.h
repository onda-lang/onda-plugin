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
  void unload();
  void requestRebuild();
  void bindBufferFile(std::string name, std::filesystem::path path);
  void clearBuffer(std::string_view name);
  void setBufferBindings(std::vector<BufferFileBinding> bindings);

  [[nodiscard]] WorkerStatus status() const;
  [[nodiscard]] std::uint64_t statusRevision() const;
  [[nodiscard]] std::optional<SeedValues> takeSeedValues();
  [[nodiscard]] PersistedProjectState persistedProjectState() const;
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
  void deactivateStatus() noexcept;
  void clearPublishedInterface() noexcept;
  void reportFailure(const char *message) noexcept;
  void collectRetired() noexcept;
  void destroy(PreparedEngine *engine) noexcept;
  [[nodiscard]] bool updateStatus(
      const Request &request, std::string message, bool compiling, bool active,
      std::optional<std::vector<ParameterMapping>> mappings = std::nullopt,
      std::optional<std::vector<BufferMapping>> buffers = std::nullopt,
      std::optional<std::vector<EventMapping>> events = std::nullopt);
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
  Request desired_;
  WorkerStatus status_;
  std::optional<SeedValues> seedValues_;
  PersistedProjectState publishedProjectState_;
  bool stopping_{};
  bool forceRebuild_{};
  std::atomic<std::uint64_t> requestGeneration_{};
  std::thread thread_;

  std::vector<std::filesystem::path> watchedPaths_;
  std::vector<std::filesystem::path> successfulPaths_;
  std::vector<FileStamp> watchedStamp_;
  std::uint64_t completedGeneration_{};
};

} // namespace onda::plugin
