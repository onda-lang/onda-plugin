#pragma once

#include "ControlState.h"
#include "EventPayload.h"
#include "OndaSdk.h"
#include "ParameterAutomation.h"
#include "ParameterMapping.h"
#include "ParameterRamp.h"
#include "ParameterState.h"
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

// Published with an engine. Audio uses these values until host seeding
// finishes.
struct SeedValues {
  // Identifies the patch selection, independent of subsequent rebuilds.
  std::uint64_t revision{};
  ParameterValues values{};
  std::size_t count{};
  ControlValues controls{};
  std::size_t controlCount{};
  ParameterSeedStamps<slotCount> parameterStamps;
  ParameterSeedStamps<onda::plugin::controlCount> controlStamps;
  std::atomic<bool> started{};
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
  bool hasProjectDefault{};
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

  [[nodiscard]] bool process(float *const *hostInputs,
                             float *const *hostOutputs, int frames,
                             std::span<const MidiEvent> midi,
                             const ParameterState &slots,
                             const HostContext &hostContext = {},
                             int hostCallbackOffset = 0) noexcept;

  // Render this callback in chunks of at most blockSize(), closing its final
  // chunk at frames. process() ranges and MIDI splits share that boundary.
  void beginHostCallback(int frames, ParameterAutomation *automation = nullptr,
                         const ControlState *controls = nullptr,
                         ControlAutomation *controlAutomation = nullptr,
                         bool useDefaults = false) noexcept;
  [[nodiscard]] bool parameterDefaultsPending() const noexcept {
    return parameterSeed_ &&
           !parameterSeed_->applied.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t parameterSeedRevision() const noexcept {
    return parameterSeed_ ? parameterSeed_->revision : 0U;
  }
  [[nodiscard]] SeedValues *parameterSeed() const noexcept {
    return parameterSeed_.get();
  }
  [[nodiscard]] std::uint64_t
  initializeControlDefaults(ControlState &controls,
                            std::uint64_t retainedLanes) noexcept;
  [[nodiscard]] bool hostCallbackUsesParameterDefaults() const noexcept {
    return hostCallbackUsesDefaults_;
  }
  [[nodiscard]] bool setParameterSmoothingSeconds(double seconds) noexcept;
  [[nodiscard]] bool setInputControlSmoothingSeconds(double seconds) noexcept;
  [[nodiscard]] bool inputControlRampPending(std::size_t control,
                                             double normalized,
                                             double seconds) const noexcept;
  [[nodiscard]] bool editorRampPending(std::size_t slot, double normalized,
                                       double seconds) const noexcept;
  [[nodiscard]] ParameterValue::Target
  controlTarget(std::size_t control, const ControlState &values) const noexcept;
  [[nodiscard]] ParameterValue::Target
  parameterTarget(std::size_t slot,
                  const ParameterState &values) const noexcept;
  void settleParameters() noexcept;

  [[nodiscard]] bool reset(const ParameterState &slots) noexcept;
  void attachLogSink(RuntimeLogSink &sink) noexcept;

  [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
  [[nodiscard]] int blockSize() const noexcept { return blockSize_; }
  [[nodiscard]] int inputChannels() const noexcept { return inputChannels_; }
  [[nodiscard]] std::uint64_t buildGeneration() const noexcept {
    return buildGeneration_;
  }
  [[nodiscard]] std::span<const ParameterMapping> parameterMappings() const {
    return {parameterMappings_.data(), parameterMappingCount_};
  }
  [[nodiscard]] std::span<const ParameterMapping> controlMappings() const {
    return controlMappings_;
  }
  [[nodiscard]] std::span<const BufferMapping> bufferMappings() const {
    return bufferMappings_;
  }
  [[nodiscard]] std::span<const EventMapping> eventMappings() const {
    return eventMappings_;
  }
  [[nodiscard]] EventTriggerResult
  triggerEvent(int index, std::span<const std::byte> payload,
               const ParameterState &slots,
               const HostContext &hostContext = {}) noexcept;
  [[nodiscard]] bool handlesMidi(MidiKind kind) const noexcept;
  [[nodiscard]] bool handlesHostContext(HostContextKind kind) const noexcept;
  [[nodiscard]] bool needsPositionInfo() const noexcept {
    return needsPositionInfo_;
  }

private:
  enum class ValueBank : std::uint8_t { parameters, controls };

  struct ResetDefault {
    std::uint64_t stamp{};
    ScalarValue value;
  };

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
  [[nodiscard]] bool prepareInputDefaults(Diagnostic &diagnostic);
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

  [[nodiscard]] bool processSegment(float *const *hostInputs,
                                    float *const *hostOutputs,
                                    int callbackOffset, int frames,
                                    const ParameterState &slots,
                                    const HostContext &hostContext,
                                    int hostCallbackOffset) noexcept;
  void prepareControlInputs(float *const *hostInputs) noexcept;
  void updateControlTarget(std::size_t control, ParameterValue::Target target,
                           const ControlValue &value,
                           bool useDefaults) noexcept;
  [[nodiscard]] ControlValue
  controlValue(std::size_t control, ParameterValue::Target target,
               const ControlState *values) const noexcept;
  [[nodiscard]] bool dispatch(const MidiEvent &event) noexcept;
  [[nodiscard]] bool ensureBlockStarted(const ParameterState &slots,
                                        const HostContext &hostContext,
                                        int hostCallbackOffset) noexcept;
  [[nodiscard]] bool dispatchHostContext(const HostContext &hostContext,
                                         int hostCallbackOffset) noexcept;
  void applyParameter(std::size_t slot, const ScalarValue &value) noexcept;
  [[nodiscard]] double writeParameterPlain(std::size_t slot,
                                           const ScalarPlain &value) noexcept;
  void applyParameters(const ParameterState &slots) noexcept;
  void updateParameters(const ParameterState &slots, int frames,
                        int offset) noexcept;
  [[nodiscard]] ScalarValue
  parameterValue(std::size_t slot, ParameterValue::Target target,
                 const ParameterState &slots) const noexcept;
  template <ValueBank Bank, std::size_t Count>
  [[nodiscard]] ScalarValue
  resolveValue(std::size_t index, ParameterValue::Target target,
               const ParameterBank<Count> *values) const noexcept;
  template <ValueBank Bank, std::size_t Count>
  [[nodiscard]] ParameterValue::Target
  resolveTarget(std::size_t index,
                const ParameterBank<Count> &values) const noexcept;

  std::uint64_t buildGeneration_{};
  // The publication owns the immutable control baseline borrowed by audio.
  // Cold metadata is read only off-thread; destruction happens on the worker
  // or during host preparation.
  std::shared_ptr<const EnginePublication> publication_;
  const HostParameterValues *checkpointValues_{}; // Owned by publication_.
  ControlDefaultStamps *initialControlStamps_{};  // Owned by publication_.
  std::shared_ptr<SeedValues> parameterSeed_;
  double sampleRate_{};
  int blockSize_{};
  ProgramHandle program_;
  int inputChannels_{};
  int outputChannels_{};
  std::vector<float> inputSlab_;
  std::array<float, static_cast<std::size_t>(pluginTotalInputChannels)>
      inputDefaults_{};
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
  int hostCallbackFrames_{-1};
  ParameterAutomation *hostAutomation_{};
  const ControlState *controlState_{};
  ControlAutomation *controlAutomation_{};
  bool controlInputsPrepared_{};
  std::array<double, controlCount> controlNormalizedTargets_{};
  std::array<ParameterRamp, controlCount> controlRamps_{};
  std::array<bool, controlCount> controlValuesStarted_{};
  std::array<bool, controlCount> controlUsesDefault_{};
  std::uint64_t controlSmoothingSamples_{};
  // Audio-owned fallback for a Reset publication overlapping a snapshot read.
  mutable ControlValues resolvedControlValues_{};
  std::vector<ParameterMapping> controlMappings_;
  std::array<double, controlCount> controlMinimum_{};
  std::array<double, controlCount> controlMaximum_{};
  bool hostCallbackUsesDefaults_{};
  bool parametersStarted_{};
  bool parametersSettleRequested_{};
  std::uint64_t smoothingSamples_{};
  std::array<double, slotCount> parameterTargets_{};
  std::array<std::optional<ScalarPlain>, slotCount> parameterPlainDefaults_{};
  mutable ParameterValues resolvedParameterValues_{};
  std::array<ParameterRamp, slotCount> parameterRamps_{};
  // Only continuous float mappings participate in editor smoothing.
  std::array<int, slotCount> smoothedParameterTypes_{};
  std::array<ParameterMapping, slotCount> parameterMappings_{};
  std::array<ScalarDomain, slotCount> parameterDomains_{};
  std::array<onda_processor_param_domain, slotCount> parameterHostDomains_{};
  std::size_t parameterMappingCount_{};
  std::array<EventBinding, static_cast<std::size_t>(MidiKind::count)>
      midiEvents_{};
  std::array<EventBinding, static_cast<std::size_t>(HostContextKind::count)>
      hostContextEvents_{};
  bool needsPositionInfo_{};

  // Keep Reset-only snapshots separate from the ordinary processing state.
  mutable std::array<ResetDefault, controlCount> controlResetDefaults_{};
  mutable std::array<ResetDefault, slotCount> parameterResetDefaults_{};

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
