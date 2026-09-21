#pragma once

#include "EventPayload.h"
#include "OndaSdk.h"
#include "Product.h"
#include "RuntimeLog.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace onda::plugin {

inline constexpr std::size_t slotCount = 32U;
using ParameterValues = std::array<float, slotCount>;

// Published with an engine. Audio uses these values until host seeding
// finishes.
struct SeedValues {
  std::uint64_t revision{};
  ParameterValues values{};
  std::size_t count{};
  std::atomic<bool> applied{};
  // Accessed only under the worker mutex.
  bool claimed{};
};

enum class MidiKind : std::uint8_t {
  noteOn,
  noteOff,
  polyPressure,
  pitchBend,
  channelPressure,
  controlChange,
  programChange,
  count,
};

enum class HostContextKind : std::uint8_t {
  transport,
  samplePosition,
  timePosition,
  tempo,
  musicalPosition,
  barPosition,
  timeSignature,
  loopRegion,
  renderMode,
  count,
};

struct MidiEvent {
  MidiKind kind{};
  std::uint32_t sampleOffset{};
  std::int32_t channel{};
  std::int32_t keyOrController{};
  float value{};
};

struct HostContext {
  struct Transport {
    bool playing{};
    bool recording{};
    bool looping{};
  };

  struct TimeSignature {
    std::int32_t numerator{};
    std::int32_t denominator{};
  };

  struct LoopRegion {
    double startQuarterNote{};
    double endQuarterNote{};
  };

  std::optional<Transport> transport;
  std::optional<std::int64_t> samplePosition;
  std::optional<double> timePosition;
  std::optional<double> tempo;
  std::optional<double> musicalPosition;
  std::optional<double> barPosition;
  std::optional<TimeSignature> timeSignature;
  std::optional<LoopRegion> loopRegion;
  bool timelinePlaying{};
  bool realtime{true};
};

struct ParameterMapping {
  int parameterIndex{-1};
  std::string name;
  std::string type;
  std::string unit;
  double defaultNormalized{0.5};
  double defaultPlain{};
  double rangeMin{};
  double rangeMax{};
  std::string scale{"linear"};
  std::optional<double> curve;
  std::optional<double> step;
  std::optional<std::int64_t> stepCount;

  friend bool operator==(const ParameterMapping &,
                         const ParameterMapping &) = default;
};

struct EventParameterMapping {
  std::string name;
  std::string type;
  PayloadTypePtr payloadType;
  std::optional<PayloadDefault> defaultValue;
};

struct EventMapping {
  int index{-1};
  std::string name;
  PayloadSchema schema;
  std::vector<EventParameterMapping> parameters;
};

enum class EventTriggerResult : std::uint8_t {
  success,
  inputRejected,
  runtimeFailure,
};

struct BufferFileBinding {
  std::string name;
  std::filesystem::path path;

  friend bool operator==(const BufferFileBinding &,
                         const BufferFileBinding &) = default;
};

enum class BufferChannelKind : std::uint8_t {
  mono,
  fixed,
  dynamic,
};

struct BufferMapping {
  int index{-1};
  std::string name;
  std::string type;
  BufferChannelKind channelKind{};
  int fixedChannels{};
  std::filesystem::path loadedPath;
  int loadedFrames{};
  int loadedChannels{};
  float loadedSampleRate{};
};

struct BuildResult;
struct EnginePublication;

class PreparedEngine final {
public:
  struct EventBinding {
    int index{-1};
    int payloadBytes{};
    std::array<int, 4U> offsets{};
  };

  ~PreparedEngine() = default;

  PreparedEngine(const PreparedEngine &) = delete;
  PreparedEngine &operator=(const PreparedEngine &) = delete;

  [[nodiscard]] static BuildResult
  build(const std::filesystem::path &path, Product product, double sampleRate,
        int blockSize, std::span<const BufferFileBinding> bufferBindings = {},
        const std::optional<ParameterValues> &initialParameters = std::nullopt);
  [[nodiscard]] static BuildResult
  build(const ProjectImage &projectImage, Product product, double sampleRate,
        int blockSize,
        const std::optional<ParameterValues> &initialParameters = std::nullopt);

  [[nodiscard]] bool
  process(float *const *hostInputs, float *const *hostOutputs, int frames,
          std::span<const MidiEvent> midi,
          const std::array<std::atomic<float> *, slotCount> &slots,
          const HostContext &hostContext = {},
          int hostCallbackOffset = 0) noexcept;

  [[nodiscard]] bool
  reset(const std::array<std::atomic<float> *, slotCount> &slots) noexcept;
  void attachLogSink(RuntimeLogSink &sink) noexcept;

  [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
  [[nodiscard]] int blockSize() const noexcept { return blockSize_; }
  [[nodiscard]] std::uint64_t buildGeneration() const noexcept {
    return buildGeneration_;
  }
  [[nodiscard]] std::span<const ParameterMapping> parameterMappings() const {
    return {parameterMappings_.data(), parameterMappingCount_};
  }
  [[nodiscard]] std::span<const BufferMapping> bufferMappings() const {
    return bufferMappings_;
  }
  [[nodiscard]] std::span<const EventMapping> eventMappings() const {
    return eventMappings_;
  }
  [[nodiscard]] EventTriggerResult
  triggerEvent(int index, std::span<const std::byte> payload,
               const std::array<std::atomic<float> *, slotCount> &slots,
               const HostContext &hostContext = {}) noexcept;
  [[nodiscard]] bool handlesMidi(MidiKind kind) const noexcept;
  [[nodiscard]] bool handlesHostContext(HostContextKind kind) const noexcept;
  [[nodiscard]] bool needsPositionInfo() const noexcept {
    return needsPositionInfo_;
  }

private:
  struct BufferStorage {
    std::vector<float> samples;
    int frames{};
    int channels{};
    float sampleRate{};
  };

  struct DelegateMetadata {
    std::string name;
    PayloadPlan plan;
  };

  struct LogSiteMetadata {
    std::string sourceFile;
    std::string lexicalOwner;
    std::uint32_t line{};
  };

  PreparedEngine(double sampleRate, int blockSize, ProgramHandle program,
                 InstanceHandle instance, int inputChannels, int outputChannels,
                 std::vector<float> inputSlab, std::vector<float> outputSlab,
                 std::vector<BufferStorage> bufferStorage,
                 std::vector<BufferMapping> bufferMappings) noexcept;
  [[nodiscard]] static BuildResult
  build(CompileResult compiled, Product product, double sampleRate,
        int blockSize, std::span<const BufferFileBinding> bufferBindings,
        const std::filesystem::path &diskEntry,
        const std::optional<ParameterValues> &initialParameters);

  [[nodiscard]] bool prepareRuntimeOutput(Diagnostic &diagnostic);
  [[nodiscard]] bool initialize() noexcept;
  void collectExecutionOutput() noexcept;
  void flushPendingLogs() noexcept;
  [[nodiscard]] bool
  publishPrint(const onda_print_occurrence_t &occurrence) noexcept;
  [[nodiscard]] bool
  publishDelegate(const onda_delegate_occurrence_t &occurrence) noexcept;
  template <typename Formatter>
  [[nodiscard]] bool publish(RuntimeLogKind kind,
                             Formatter &&formatter) noexcept;
  void addGeneratedOverflow(RuntimeLogKind kind, std::uint32_t count) noexcept;
  void addTransportDrop(RuntimeLogKind kind, std::uint64_t count = 1U) noexcept;

  template <typename... Values>
  [[nodiscard]] bool trigger(const EventBinding &binding,
                             Values... values) noexcept;

  [[nodiscard]] bool processSegment(
      float *const *hostInputs, float *const *hostOutputs, int callbackOffset,
      int frames, const std::array<std::atomic<float> *, slotCount> &slots,
      const HostContext &hostContext, int hostCallbackOffset) noexcept;
  [[nodiscard]] bool dispatch(const MidiEvent &event) noexcept;
  [[nodiscard]] bool
  ensureBlockStarted(const std::array<std::atomic<float> *, slotCount> &slots,
                     const HostContext &hostContext,
                     int hostCallbackOffset) noexcept;
  [[nodiscard]] bool dispatchHostContext(const HostContext &hostContext,
                                         int hostCallbackOffset) noexcept;
  void applyParameter(std::size_t slot, float value) noexcept;
  void applyParameters(
      const std::array<std::atomic<float> *, slotCount> &slots) noexcept;

  std::uint64_t buildGeneration_{};
  // Worker metadata lives until this engine is retired. Audio never accesses
  // or copies it; destruction happens on the worker or during host preparation.
  std::shared_ptr<const EnginePublication> publication_;
  std::shared_ptr<SeedValues> parameterSeed_;
  double sampleRate_{};
  int blockSize_{};
  ProgramHandle program_;
  int inputChannels_{};
  int outputChannels_{};
  std::vector<float> inputSlab_;
  std::vector<float> outputSlab_;
  std::vector<BufferStorage> bufferStorage_;
  // Onda instances retain the bound slab and buffer pointers. Declaring the
  // instance after their storage makes it the first of these members destroyed.
  InstanceHandle instance_;
  std::vector<BufferMapping> bufferMappings_;
  std::vector<EventMapping> eventMappings_;
  std::vector<DelegateMetadata> delegates_;
  std::vector<LogSiteMetadata> logSites_;
  std::vector<std::uint8_t> delegateBatchStorage_;
  std::vector<std::uint8_t> printBatchStorage_;
  onda_delegate_batch_t delegateBatch_{};
  onda_print_batch_t printBatch_{};
  onda_execution_output_t executionOutput_{};
  RuntimeLogSink *logSink_{};
  std::vector<RuntimeLogEntry> pendingLogs_;
  std::size_t pendingLogRead_{};
  RuntimeLogCounters pendingLogCounters_{};
  int logicalFrame_{};
  bool blockStarted_{};
  std::array<ParameterMapping, slotCount> parameterMappings_{};
  std::size_t parameterMappingCount_{};
  std::array<EventBinding, static_cast<std::size_t>(MidiKind::count)>
      midiEvents_{};
  std::array<EventBinding, static_cast<std::size_t>(HostContextKind::count)>
      hostContextEvents_{};
  bool needsPositionInfo_{};

  friend struct BuildResult;
  friend class Worker;
};

struct BuildResult {
  std::unique_ptr<PreparedEngine> engine;
  ProjectImage projectImage;
  std::vector<std::filesystem::path> watchPaths;
  std::vector<BufferMapping> buffers;
  Diagnostic diagnostic;
};

} // namespace onda::plugin
