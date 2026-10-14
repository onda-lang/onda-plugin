#include "Engine.h"

#include "AudioFile.h"
#include "Primitive.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace onda::plugin {
namespace {

static_assert(sizeof(float) == 4U);
static_assert(sizeof(double) == 8U);
static_assert(sizeof(std::int32_t) == 4U);
static_assert(sizeof(std::int64_t) == 8U);

constexpr std::size_t runtimeBatchCapacity = 64U * 1024U;
constexpr std::size_t maximumPendingLogEntries = runtimeLogQueueCapacity - 1U;

template <typename Unsigned>
Unsigned readLittleEndian(const std::uint8_t *bytes) noexcept {
  Unsigned value{};
  for (std::size_t index = 0; index < sizeof(Unsigned); ++index)
    value |= static_cast<Unsigned>(bytes[index]) << (index * 8U);
  return value;
}

Diagnostic error(std::string message) {
  Diagnostic result;
  result.message = std::move(message);
  return result;
}

int flattenedChannels(const onda_program_t *program, const bool input,
                      const int maximumChannels, Diagnostic &diagnostic) {
  const auto count =
      input ? onda_input_count(program) : onda_output_count(program);
  if (count < 0) {
    diagnostic = error("Onda returned invalid audio interface metadata");
    return -1;
  }

  int channels = 0;
  for (int index = 0; index < count; ++index) {
    const auto primitive = input ? onda_input_elem_type(program, index)
                                 : onda_output_elem_type(program, index);
    const auto arrayLength = input ? onda_input_array_len(program, index)
                                   : onda_output_array_len(program, index);
    if (primitive != ONDA_PRIMITIVE_F32 || arrayLength <= 0 ||
        channels > maximumChannels - arrayLength) {
      diagnostic = error("Audio " + std::string(input ? "inputs" : "outputs") +
                         " must flatten to at most " +
                         std::to_string(maximumChannels) + " f32 channels");
      return -1;
    }
    channels += arrayLength;
  }
  return channels;
}

bool bindAudio(onda_instance_t *instance, const onda_program_t *program,
               std::vector<float> &slab, const bool input, const int blockSize,
               Diagnostic &diagnostic) {
  const auto count =
      input ? onda_input_count(program) : onda_output_count(program);
  for (int index = 0; index < count; ++index) {
    const auto slotOffset = input ? onda_input_slot_offset(program, index)
                                  : onda_output_slot_offset(program, index);
    const auto arrayLength = input ? onda_input_array_len(program, index)
                                   : onda_output_array_len(program, index);
    const auto availableChannels =
        slab.size() / static_cast<std::size_t>(blockSize);
    if (slotOffset < 0 || arrayLength <= 0 ||
        static_cast<std::size_t>(slotOffset) +
                static_cast<std::size_t>(arrayLength) >
            availableChannels) {
      diagnostic = error("Onda returned invalid flattened audio metadata");
      return false;
    }
    const auto samples = static_cast<std::size_t>(arrayLength) *
                         static_cast<std::size_t>(blockSize);
    if (samples > static_cast<std::size_t>(std::numeric_limits<int>::max()) /
                      sizeof(float)) {
      diagnostic = error("The prepared audio slab is too large");
      return false;
    }
    auto *pointer = slab.data() + static_cast<std::size_t>(slotOffset) *
                                      static_cast<std::size_t>(blockSize);
    const auto bytes = static_cast<int>(samples * sizeof(float));
    const auto status = input
                            ? onda_bind_input(instance, index, pointer, bytes)
                            : onda_bind_output(instance, index, pointer, bytes);
    if (status != 0) {
      diagnostic = error(input ? "Failed to bind an Onda input slab"
                               : "Failed to bind an Onda output slab");
      return false;
    }
  }
  return true;
}

std::optional<std::string> parameterUnit(const onda_program_t *program,
                                         const int index) {
  const auto bytes = onda_param_unit_copy(program, index, nullptr, 0);
  if (bytes < 0)
    return std::nullopt;
  if (bytes == 0)
    return std::string{};

  std::vector<char> buffer(static_cast<std::size_t>(bytes));
  if (onda_param_unit_copy(program, index, buffer.data(), bytes) != bytes ||
      buffer.back() != '\0') {
    return std::nullopt;
  }
  return std::string(buffer.data(), buffer.size() - 1U);
}

std::optional<ParameterMapping> parameterMapping(const onda_program_t *program,
                                                 const int index) {
  if (onda_param_array_len(program, index) != 1)
    return std::nullopt;
  const auto *declaredType = onda_param_type(program, index);
  if (declaredType == nullptr ||
      std::string_view{declaredType}.find('[') != std::string_view::npos)
    return std::nullopt;
  const auto primitive = onda_param_elem_type(program, index);
  const auto type = primitiveName(primitive);
  const auto unit = parameterUnit(program, index);
  if (type.empty() || !unit)
    return std::nullopt;

  ParameterMapping mapping;
  mapping.parameterIndex = index;
  mapping.type = type;
  mapping.unit = *unit;
  if (const auto *name = onda_param_name(program, index))
    mapping.name = name;

  const auto ranged = onda_param_has_range(program, index) == 1;
  mapping.defaultMayExceedRange = !ranged && primitive != ONDA_PRIMITIVE_BOOL;
  // This is the plugin's control domain; it adds no range to Onda's storage.
  mapping.rangeMin = ranged ? onda_param_range_min_f64(program, index) : 0.0;
  mapping.rangeMax = ranged ? onda_param_range_max_f64(program, index) : 1.0;
  if (!std::isfinite(mapping.rangeMin) || !std::isfinite(mapping.rangeMax) ||
      mapping.rangeMin >= mapping.rangeMax) {
    return std::nullopt;
  }

  const auto scale =
      ranged ? onda_param_scale(program, index) : ONDA_PARAM_SCALE_LINEAR;
  if (scale == ONDA_PARAM_SCALE_LINEAR)
    mapping.scale = "linear";
  else if (scale == ONDA_PARAM_SCALE_LOG)
    mapping.scale = "log";
  else
    return std::nullopt;

  const auto hasCurve = onda_param_has_curve(program, index);
  if (hasCurve < 0)
    return std::nullopt;
  if (hasCurve == 1) {
    const auto curve = onda_param_curve(program, index);
    if (!std::isfinite(curve))
      return std::nullopt;
    mapping.curve = curve;
  }

  const auto hasStep = onda_param_has_step(program, index);
  if (hasStep < 0)
    return std::nullopt;
  if (hasStep == 1) {
    const auto step = onda_param_step_f64(program, index);
    const auto stepCount = onda_param_step_count(program, index);
    if (!std::isfinite(step) || step <= 0.0 || stepCount <= 0)
      return std::nullopt;
    mapping.step = step;
    mapping.stepCount = stepCount;
  } else if (!ranged && (primitive == ONDA_PRIMITIVE_I32 ||
                         primitive == ONDA_PRIMITIVE_I64)) {
    mapping.step = 1.0;
    mapping.stepCount = 1;
  }

  const auto hasDefault = onda_param_has_default(program, index) == 1;
  if (primitive == ONDA_PRIMITIVE_I64 && hasDefault) {
    // Unranged i64 defaults retain the full integer width through the raw API.
    std::array<std::uint8_t, sizeof(std::int64_t)> bytes{};
    if (onda_param_default_bytes(program, index, bytes.data(),
                                 static_cast<int>(bytes.size())) !=
        static_cast<int>(bytes.size()))
      return std::nullopt;
    mapping.defaultPlain =
        std::bit_cast<std::int64_t>(readLittleEndian<std::uint64_t>(bytes.data()));
  } else {
    mapping.defaultPlain =
        hasDefault ? onda_param_default_f64(program, index)
                   : std::clamp(0.0, mapping.rangeMin, mapping.rangeMax);
  }
  const auto defaultNumber = scalarPlainNumber(mapping.defaultPlain);
  if (!std::isfinite(defaultNumber))
    return std::nullopt;
  mapping.defaultNormalized = mapping.plainToNormalized(defaultNumber);
  if (!std::isfinite(mapping.defaultNormalized) ||
      !std::isfinite(mapping.normalizedToPlain(0.5)))
    return std::nullopt;
  return mapping;
}

struct ExpectedEvent {
  std::string_view name;
  std::span<const std::string_view> parameterNames;
  std::span<const int> parameterTypes;
  std::string_view category;
  bool requiredForInstrument{};
};

static_assert(static_cast<std::size_t>(HostContextKind::count) == 9U);

constexpr std::array<std::string_view, 4U> noteNames{"id", "channel", "key",
                                                     "velocity"};
constexpr std::array<int, 4U> noteTypes{ONDA_PRIMITIVE_I32, ONDA_PRIMITIVE_I32,
                                        ONDA_PRIMITIVE_I32, ONDA_PRIMITIVE_F32};
constexpr std::array<std::string_view, 2U> pitchNames{"channel", "value"};
constexpr std::array<int, 2U> pitchTypes{ONDA_PRIMITIVE_I32,
                                         ONDA_PRIMITIVE_F32};
constexpr std::array<std::string_view, 2U> pressureNames{"channel", "pressure"};
constexpr std::array<int, 2U> pressureTypes{ONDA_PRIMITIVE_I32,
                                            ONDA_PRIMITIVE_F32};
constexpr std::array<std::string_view, 3U> ccNames{"channel", "index", "value"};
constexpr std::array<int, 3U> ccTypes{ONDA_PRIMITIVE_I32, ONDA_PRIMITIVE_I32,
                                      ONDA_PRIMITIVE_F32};
constexpr std::array<std::string_view, 3U> polyPressureNames{"channel", "key",
                                                             "pressure"};
constexpr std::array<int, 3U> polyPressureTypes{
    ONDA_PRIMITIVE_I32, ONDA_PRIMITIVE_I32, ONDA_PRIMITIVE_F32};
constexpr std::array<std::string_view, 2U> programChangeNames{"channel",
                                                              "program"};
constexpr std::array<int, 2U> programChangeTypes{ONDA_PRIMITIVE_I32,
                                                 ONDA_PRIMITIVE_I32};

constexpr std::array<std::string_view, 3U> transportNames{
    "playing", "recording", "looping"};
constexpr std::array<int, 3U> transportTypes{
    ONDA_PRIMITIVE_BOOL, ONDA_PRIMITIVE_BOOL, ONDA_PRIMITIVE_BOOL};
constexpr std::array<std::string_view, 1U> samplePositionNames{"sample"};
constexpr std::array<int, 1U> samplePositionTypes{ONDA_PRIMITIVE_I64};
constexpr std::array<std::string_view, 1U> timePositionNames{"seconds"};
constexpr std::array<int, 1U> timePositionTypes{ONDA_PRIMITIVE_F64};
constexpr std::array<std::string_view, 1U> tempoNames{"bpm"};
constexpr std::array<int, 1U> tempoTypes{ONDA_PRIMITIVE_F64};
constexpr std::array<std::string_view, 1U> musicalPositionNames{"quarter_note"};
constexpr std::array<int, 1U> musicalPositionTypes{ONDA_PRIMITIVE_F64};
constexpr std::array<std::string_view, 1U> barPositionNames{
    "start_quarter_note"};
constexpr std::array<int, 1U> barPositionTypes{ONDA_PRIMITIVE_F64};
constexpr std::array<std::string_view, 2U> timeSignatureNames{"numerator",
                                                              "denominator"};
constexpr std::array<int, 2U> timeSignatureTypes{ONDA_PRIMITIVE_I32,
                                                 ONDA_PRIMITIVE_I32};
constexpr std::array<std::string_view, 2U> loopRegionNames{"start_quarter_note",
                                                           "end_quarter_note"};
constexpr std::array<int, 2U> loopRegionTypes{ONDA_PRIMITIVE_F64,
                                              ONDA_PRIMITIVE_F64};
constexpr std::array<std::string_view, 1U> renderModeNames{"realtime"};
constexpr std::array<int, 1U> renderModeTypes{ONDA_PRIMITIVE_BOOL};

constexpr std::array<ExpectedEvent, static_cast<std::size_t>(MidiKind::count)>
    expectedMidiEvents{{
        {"note_on", noteNames, noteTypes, "MIDI", true},
        {"note_off", noteNames, noteTypes, "MIDI", true},
        {"poly_pressure", polyPressureNames, polyPressureTypes, "MIDI"},
        {"pitch_bend", pitchNames, pitchTypes, "MIDI"},
        {"channel_pressure", pressureNames, pressureTypes, "MIDI"},
        {"cc", ccNames, ccTypes, "MIDI"},
        {"program_change", programChangeNames, programChangeTypes, "MIDI"},
    }};

constexpr std::array<ExpectedEvent,
                     static_cast<std::size_t>(HostContextKind::count)>
    expectedHostContextEvents{{
        {"transport", transportNames, transportTypes, "host-context"},
        {"sample_position", samplePositionNames, samplePositionTypes,
         "host-context"},
        {"time_position", timePositionNames, timePositionTypes, "host-context"},
        {"tempo", tempoNames, tempoTypes, "host-context"},
        {"musical_position", musicalPositionNames, musicalPositionTypes,
         "host-context"},
        {"bar_position", barPositionNames, barPositionTypes, "host-context"},
        {"time_signature", timeSignatureNames, timeSignatureTypes,
         "host-context"},
        {"loop_region", loopRegionNames, loopRegionTypes, "host-context"},
        {"render_mode", renderModeNames, renderModeTypes, "host-context"},
    }};

bool isPluginEvent(const std::string_view name) noexcept {
  const auto named = [name](const ExpectedEvent &event) {
    return event.name == name;
  };
  return std::ranges::any_of(expectedMidiEvents, named) ||
         std::ranges::any_of(expectedHostContextEvents, named);
}

int payloadPrimitive(const PayloadScalar scalar) noexcept {
  switch (scalar) {
  case PayloadScalar::f32:
    return ONDA_PRIMITIVE_F32;
  case PayloadScalar::f64:
    return ONDA_PRIMITIVE_F64;
  case PayloadScalar::i32:
    return ONDA_PRIMITIVE_I32;
  case PayloadScalar::i64:
    return ONDA_PRIMITIVE_I64;
  case PayloadScalar::boolean:
    return ONDA_PRIMITIVE_BOOL;
  }
  return -1;
}

bool eventTensorMetadataMatches(const onda_program_t *program,
                                const int eventIndex,
                                const PayloadSchema &schema) {
  const auto tensorCount = onda_event_tensor_count(program, eventIndex);
  if (tensorCount < 0)
    return false;

  const auto plan = makePayloadPlan(schema);
  const auto expectedCount = std::accumulate(
      plan.parameters.begin(), plan.parameters.end(), std::size_t{},
      [](const auto count, const auto &parameter) {
        return count + parameter.leaves.size();
      });
  if (expectedCount != static_cast<std::size_t>(tensorCount))
    return false;

  int tensorIndex{};
  for (std::size_t parameterIndex = 0; parameterIndex < plan.parameters.size();
       ++parameterIndex) {
    const auto &parameter = plan.parameters[parameterIndex];
    for (const auto &leaf : parameter.leaves) {
      onda_event_tensor_info_t info{};
      if (onda_event_tensor_info(program, eventIndex, tensorIndex++, &info) !=
              0 ||
          info.path == nullptr || info.shape_rank < 0 ||
          info.parameter_index != static_cast<int>(parameterIndex) ||
          info.element_type != payloadPrimitive(leaf.scalar) ||
          info.is_slice != static_cast<int>(parameter.dynamic) ||
          info.fixed_element_count <= 0 ||
          static_cast<std::size_t>(info.fixed_element_count) !=
              leaf.fixedElements ||
          leaf.path != info.path ||
          static_cast<std::size_t>(info.shape_rank) != leaf.shape.size() ||
          (info.shape_rank != 0 && info.shape == nullptr)) {
        return false;
      }
      for (int axis = 0; axis < info.shape_rank; ++axis) {
        if (info.shape[axis] <= 0 ||
            static_cast<std::size_t>(info.shape[axis]) !=
                leaf.shape[static_cast<std::size_t>(axis)]) {
          return false;
        }
      }
    }
  }
  return true;
}

bool collectVisibleEvents(const onda_program_t *program,
                          std::vector<EventMapping> &events,
                          Diagnostic &diagnostic) {
  const auto count = onda_event_count(program);
  if (count < 0) {
    diagnostic = error("Onda returned invalid event metadata");
    return false;
  }
  events.reserve(static_cast<std::size_t>(count));
  for (int eventIndex = 0; eventIndex < count; ++eventIndex) {
    const auto *rawName = onda_event_name(program, eventIndex);
    const auto *schemaJson = onda_event_schema_json(program, eventIndex);
    if (rawName == nullptr || schemaJson == nullptr) {
      diagnostic = error("Onda returned invalid event metadata");
      return false;
    }
    if (isPluginEvent(rawName))
      continue;

    EventMapping event{
        .index = eventIndex,
        .name = rawName,
        .schema = {},
        .parameters = {},
    };
    std::string schemaError;
    if (!parsePayloadSchema(schemaJson, event.schema, schemaError)) {
      diagnostic =
          error("Onda returned invalid event metadata: " + schemaError);
      return false;
    }
    if (!eventTensorMetadataMatches(program, eventIndex, event.schema)) {
      diagnostic = error("Onda returned inconsistent event tensor metadata");
      return false;
    }
    event.parameters.reserve(event.schema.parameters.size());
    for (const auto &parameter : event.schema.parameters) {
      event.parameters.push_back({
          .name = parameter.name,
          .type = payloadTypeName(*parameter.type),
          .payloadType = parameter.type,
          .defaultValue = parameter.defaultValue,
      });
    }
    events.push_back(std::move(event));
  }
  return true;
}

bool appendText(RuntimeLogEntry &entry, const std::string_view text) noexcept {
  const auto available = entry.text.size() - entry.textBytes;
  if (text.size() > available)
    return false;
  std::memcpy(entry.text.data() + entry.textBytes, text.data(), text.size());
  entry.textBytes += static_cast<std::uint32_t>(text.size());
  return true;
}

template <typename Value>
bool appendNumber(RuntimeLogEntry &entry, const Value value) noexcept {
  std::array<char, 64U> text{};
  const auto converted =
      std::to_chars(text.data(), text.data() + text.size(), value);
  return converted.ec == std::errc{} &&
         appendText(entry, {text.data(), converted.ptr});
}

bool appendPayloadScalar(RuntimeLogEntry &entry, const PayloadScalar scalar,
                         const std::uint8_t *bytes) noexcept {
  switch (scalar) {
  case PayloadScalar::boolean:
    return appendText(entry, bytes[0] == 0U ? "false" : "true");
  case PayloadScalar::f32:
    return appendNumber(
        entry, std::bit_cast<float>(readLittleEndian<std::uint32_t>(bytes)));
  case PayloadScalar::f64:
    return appendNumber(
        entry, std::bit_cast<double>(readLittleEndian<std::uint64_t>(bytes)));
  case PayloadScalar::i32:
    return appendNumber(entry, std::bit_cast<std::int32_t>(
                                   readLittleEndian<std::uint32_t>(bytes)));
  case PayloadScalar::i64:
    return appendNumber(entry, std::bit_cast<std::int64_t>(
                                   readLittleEndian<std::uint64_t>(bytes)));
  }
  return false;
}

template <std::size_t Size>
std::uint32_t copyText(std::array<char, Size> &destination,
                       const std::string_view source) noexcept {
  auto bytes = std::min(source.size(), destination.size());
  if (bytes < source.size()) {
    while (bytes != 0U &&
           (static_cast<unsigned char>(source[bytes]) & 0xc0U) == 0x80U) {
      --bytes;
    }
  }
  std::memcpy(destination.data(), source.data(), bytes);
  return static_cast<std::uint32_t>(bytes);
}

bool validateEvent(const onda_program_t *program, const ExpectedEvent &expected,
                   const Product product, PreparedEngine::EventBinding &binding,
                   Diagnostic &diagnostic) {
  const std::string name(expected.name);
  const auto index = onda_event_index(program, name.c_str());
  if (index < 0) {
    if (product == Product::instrument && expected.requiredForInstrument) {
      diagnostic =
          error("The instrument requires exact note_on and note_off events");
      return false;
    }
    return true;
  }

  const auto parameterCount = onda_event_param_count(program, index);
  if (parameterCount != static_cast<int>(expected.parameterNames.size()) ||
      expected.parameterNames.size() > binding.offsets.size()) {
    diagnostic = error("Canonical " + std::string(expected.category) +
                       " event '" + name + "' has an incompatible payload");
    return false;
  }

  for (int parameter = 0; parameter < parameterCount; ++parameter) {
    const auto *parameterName =
        onda_event_param_name(program, index, parameter);
    const auto arrayLength =
        onda_event_param_array_len(program, index, parameter);
    if (parameterName == nullptr ||
        expected.parameterNames[static_cast<std::size_t>(parameter)] !=
            parameterName ||
        onda_event_param_elem_type(program, index, parameter) !=
            expected.parameterTypes[static_cast<std::size_t>(parameter)] ||
        onda_event_param_is_slice(program, index, parameter) != 0 ||
        onda_event_param_is_array(program, index, parameter) != 0 ||
        arrayLength != 1) {
      diagnostic = error("Canonical " + std::string(expected.category) +
                         " event '" + name + "' has an incompatible payload");
      return false;
    }
    binding.offsets[static_cast<std::size_t>(parameter)] =
        onda_event_param_offset_bytes(program, index, parameter);
  }

  binding.index = index;
  binding.payloadBytes = onda_event_payload_bytes(program, index);
  if (binding.payloadBytes <= 0 || binding.payloadBytes > 32) {
    diagnostic = error("Canonical " + std::string(expected.category) +
                       " event '" + name + "' has an invalid packed payload");
    return false;
  }
  for (int parameter = 0; parameter < parameterCount; ++parameter) {
    const auto offset = binding.offsets[static_cast<std::size_t>(parameter)];
    const auto bytes = primitiveBytes(
        expected.parameterTypes[static_cast<std::size_t>(parameter)]);
    if (bytes == 0U || offset < 0 ||
        static_cast<std::size_t>(offset) + bytes >
            static_cast<std::size_t>(binding.payloadBytes)) {
      diagnostic = error("Canonical " + std::string(expected.category) +
                         " event '" + name + "' has an invalid packed payload");
      return false;
    }
  }
  return true;
}

template <typename Value>
bool writePayload(std::array<std::byte, 32U> &payload, const int payloadBytes,
                  const int offset, const Value value) noexcept {
  if (offset < 0 || static_cast<std::size_t>(offset) + sizeof(Value) >
                        static_cast<std::size_t>(payloadBytes)) {
    return false;
  }
  const auto bits = [&] {
    if constexpr (std::is_same_v<Value, bool>)
      return static_cast<std::uint8_t>(value);
    else if constexpr (std::is_same_v<Value, float>)
      return std::bit_cast<std::uint32_t>(value);
    else if constexpr (std::is_same_v<Value, double>)
      return std::bit_cast<std::uint64_t>(value);
    else
      return std::bit_cast<std::make_unsigned_t<Value>>(value);
  }();
  for (std::size_t byte = 0; byte < sizeof(bits); ++byte) {
    payload[static_cast<std::size_t>(offset) + byte] =
        static_cast<std::byte>((bits >> (byte * 8U)) & decltype(bits){0xff});
  }
  return true;
}

} // namespace

template <typename... Values>
bool PreparedEngine::trigger(const EventBinding &binding,
                             Values... values) noexcept {
  if (binding.index < 0)
    return true;
  std::array<std::byte, 32U> payload{};
  std::size_t index = 0U;
  const auto valid = (writePayload(payload, binding.payloadBytes,
                                   binding.offsets[index++], values) &&
                      ...);
  if (!valid)
    return false;
  const auto status = onda_trigger_event_by_index_unchecked(
      instance_.get(), binding.index, payload.data(), binding.payloadBytes,
      &executionOutput_);
  collectExecutionOutput();
  return status == 0;
}

PreparedEngine::PreparedEngine(
    const double sampleRate, const int blockSize, ProgramHandle program,
    InstanceHandle instance, const int inputChannels, const int outputChannels,
    std::vector<float> inputSlab, std::vector<float> outputSlab,
    std::vector<BufferStorage> bufferStorage,
    std::vector<BufferMapping> bufferMappings) noexcept
    : sampleRate_(sampleRate), blockSize_(blockSize),
      program_(std::move(program)), inputChannels_(inputChannels),
      outputChannels_(outputChannels), inputSlab_(std::move(inputSlab)),
      outputSlab_(std::move(outputSlab)),
      bufferStorage_(std::move(bufferStorage)), instance_(std::move(instance)),
      bufferMappings_(std::move(bufferMappings)) {
  smoothedParameterTypes_.fill(-1);
}

bool PreparedEngine::prepareInputDefaults(Diagnostic &diagnostic) {
  const auto controls = static_cast<std::size_t>(
      std::max(inputChannels_ - pluginInputChannels, 0));
  controlMappings_.reserve(controls);
  for (int index = 0; index < onda_input_count(program_.get()); ++index) {
    const auto offset = onda_input_slot_offset(program_.get(), index);
    const auto length = onda_input_array_len(program_.get(), index);
    if (offset < 0 || length <= 0 || offset > inputChannels_ - length) {
      diagnostic = error("Onda returned invalid flattened input metadata");
      return false;
    }
    if (offset + length <= pluginInputChannels)
      continue;
    const auto *name = onda_input_name(program_.get(), index);
    const auto *type = onda_input_type(program_.get(), index);
    if (name == nullptr || type == nullptr) {
      diagnostic = error("Onda returned invalid input names/types");
      return false;
    }
    const auto array =
        std::string_view{type}.find('[') != std::string_view::npos;
    const auto hasDefault = onda_input_has_default(program_.get(), index) == 1;
    const auto value =
        hasDefault ? onda_input_default_f64(program_.get(), index) : 0.0;
    // The SDK exposes scalar defaults only. Array defaults return NaN.
    if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value))) {
      diagnostic = error("External control input '" + std::string{name} +
                         "' defaults must be finite scalars; declare separate "
                         "scalar inputs for array defaults");
      return false;
    }
    const auto ranged = onda_input_has_range(program_.get(), index) == 1;
    // Unranged controls use a unit interval extended to include their default.
    const auto minimum = ranged
                             ? onda_input_range_min_f64(program_.get(), index)
                             : std::min(0.0, value);
    const auto maximum = ranged
                             ? onda_input_range_max_f64(program_.get(), index)
                             : std::max(1.0, value);
    if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
        minimum > maximum || !std::isfinite(static_cast<float>(minimum)) ||
        !std::isfinite(static_cast<float>(maximum))) {
      diagnostic =
          error("External control input ranges must be finite f32 ranges");
      return false;
    }
    const auto defaultPlain = std::clamp(value, minimum, maximum);
    const auto normalized = maximum > minimum
                                ? (defaultPlain - minimum) / (maximum - minimum)
                                : 0.0;
    for (int element = std::max(pluginInputChannels - offset, 0);
         element < length; ++element) {
      const auto channel = offset + element;
      const auto control =
          static_cast<std::size_t>(channel - pluginInputChannels);
      if (control != controlMappings_.size()) {
        diagnostic =
            error("Onda returned invalid flattened control input metadata");
        return false;
      }
      inputDefaults_[static_cast<std::size_t>(channel)] =
          static_cast<float>(defaultPlain);
      controlMinimum_[control] = minimum;
      controlMaximum_[control] = maximum;
      ParameterMapping mapping;
      mapping.name = std::string{name} +
                     (array ? "[" + std::to_string(element) + "]" : "");
      mapping.type = "f32";
      mapping.defaultNormalized = normalized;
      mapping.defaultPlain = defaultPlain;
      mapping.rangeMin = minimum;
      mapping.rangeMax = maximum;
      resolvedControlValues_[control] = mapping.controlDefault();
      controlMappings_.push_back(std::move(mapping));
    }
  }
  return true;
}

bool PreparedEngine::prepareRuntimeOutput(Diagnostic &diagnostic) {
  const auto delegateCount = onda_delegate_count(program_.get());
  const auto logSiteCount = onda_log_site_count(program_.get());
  if (delegateCount < 0 || logSiteCount < 0) {
    diagnostic = error("Onda returned invalid runtime-output metadata");
    return false;
  }

  delegates_.reserve(static_cast<std::size_t>(delegateCount));
  for (int delegateIndex = 0; delegateIndex < delegateCount; ++delegateIndex) {
    const auto *name = onda_delegate_name(program_.get(), delegateIndex);
    const auto *schemaJson =
        onda_delegate_schema_json(program_.get(), delegateIndex);
    if (name == nullptr || schemaJson == nullptr) {
      diagnostic = error("Onda returned invalid delegate metadata");
      return false;
    }
    PayloadSchema schema;
    std::string schemaError;
    if (!parsePayloadSchema(schemaJson, schema, schemaError)) {
      diagnostic =
          error("Onda returned invalid delegate metadata: " + schemaError);
      return false;
    }
    delegates_.push_back({.name = name, .plan = makePayloadPlan(schema)});
  }

  logSites_.reserve(static_cast<std::size_t>(logSiteCount));
  for (int index = 0; index < logSiteCount; ++index) {
    onda_log_site_info_t info{};
    if (onda_log_site_info(program_.get(), index, &info) != 0) {
      diagnostic = error("Onda returned invalid print-site metadata");
      return false;
    }
    LogSiteMetadata site;
    if (info.source.file_index >= 0) {
      const auto *path =
          onda_source_file_path(program_.get(), info.source.file_index);
      if (path == nullptr) {
        diagnostic = error("Onda returned invalid print-site source metadata");
        return false;
      }
      site.sourceFile = path;
    }
    if (info.lexical_owner != nullptr)
      site.lexicalOwner = info.lexical_owner;
    site.line = info.source.line;
    logSites_.push_back(std::move(site));
  }

  if (!delegates_.empty())
    delegateBatchStorage_.resize(runtimeBatchCapacity);
  if (!logSites_.empty())
    printBatchStorage_.resize(runtimeBatchCapacity);
  delegateBatch_ = {
      .storage = delegateBatchStorage_.empty() ? nullptr
                                               : delegateBatchStorage_.data(),
      .capacity_bytes =
          static_cast<std::uint32_t>(delegateBatchStorage_.size()),
      .used_bytes = 0U,
      .record_count = 0U,
      .overflow_count = 0U,
  };
  printBatch_ = {
      .storage =
          printBatchStorage_.empty() ? nullptr : printBatchStorage_.data(),
      .capacity_bytes = static_cast<std::uint32_t>(printBatchStorage_.size()),
      .used_bytes = 0U,
      .record_count = 0U,
      .overflow_count = 0U,
  };
  executionOutput_ = {
      .delegate_batch = delegates_.empty() ? nullptr : &delegateBatch_,
      .print_batch = logSites_.empty() ? nullptr : &printBatch_,
  };
  if (!delegates_.empty() || !logSites_.empty())
    pendingLogs_.reserve(maximumPendingLogEntries);
  return true;
}

bool PreparedEngine::initialize() noexcept {
  const auto status =
      onda_init_checked(instance_.get(), ONDA_INIT_FULL, &executionOutput_);
  collectExecutionOutput();
  return status == 0;
}

void PreparedEngine::attachLogSink(RuntimeLogSink &sink) noexcept {
  if (logSink_ != nullptr)
    return;
  logSink_ = &sink;
  sink.addGeneratedOverflow(RuntimeLogKind::print,
                            pendingLogCounters_.printOverflow);
  sink.addGeneratedOverflow(RuntimeLogKind::delegate,
                            pendingLogCounters_.delegateOverflow);
  sink.addTransportDrops(RuntimeLogKind::print,
                         pendingLogCounters_.printTransportDrops);
  sink.addTransportDrops(RuntimeLogKind::delegate,
                         pendingLogCounters_.delegateTransportDrops);
  flushPendingLogs();
}

void PreparedEngine::flushPendingLogs() noexcept {
  if (logSink_ == nullptr)
    return;
  while (pendingLogRead_ < pendingLogs_.size() &&
         logSink_->tryPush(pendingLogs_[pendingLogRead_], buildGeneration_)) {
    ++pendingLogRead_;
  }
}

void PreparedEngine::addGeneratedOverflow(const RuntimeLogKind kind,
                                          const std::uint32_t count) noexcept {
  if (count == 0U)
    return;
  if (logSink_ != nullptr) {
    logSink_->addGeneratedOverflow(kind, count);
  } else if (kind == RuntimeLogKind::print) {
    addSaturated(pendingLogCounters_.printOverflow, count);
  } else {
    addSaturated(pendingLogCounters_.delegateOverflow, count);
  }
}

void PreparedEngine::addTransportDrop(const RuntimeLogKind kind,
                                      const std::uint64_t count) noexcept {
  if (count == 0U)
    return;
  if (logSink_ != nullptr) {
    logSink_->addTransportDrops(kind, count);
  } else if (kind == RuntimeLogKind::print) {
    addSaturated(pendingLogCounters_.printTransportDrops, count);
  } else {
    addSaturated(pendingLogCounters_.delegateTransportDrops, count);
  }
}

template <typename Formatter>
bool PreparedEngine::publish(const RuntimeLogKind kind,
                             Formatter &&formatter) noexcept {
  if (logSink_ != nullptr) {
    return logSink_->tryEmplace(kind, buildGeneration_,
                                std::forward<Formatter>(formatter));
  }
  if (pendingLogs_.size() >= maximumPendingLogEntries)
    return false;
  RuntimeLogEntry entry;
  entry.prepare(kind, buildGeneration_);
  if (!std::forward<Formatter>(formatter)(entry))
    return false;
  pendingLogs_.push_back(std::move(entry));
  return true;
}

bool PreparedEngine::publishPrint(
    const onda_print_occurrence_t &occurrence) noexcept {
  if (occurrence.site_index >= logSites_.size() ||
      occurrence.payload == nullptr)
    return false;
  const auto recordBytes =
      static_cast<std::uint64_t>(ONDA_PRINT_RECORD_HEADER_SIZE) +
      occurrence.payload_size_bytes;
  if (recordBytes > std::numeric_limits<std::uint32_t>::max())
    return false;
  return publish(RuntimeLogKind::print, [this, &occurrence, recordBytes](
                                            RuntimeLogEntry &entry) noexcept {
    onda_print_batch_t batch{
        .storage = const_cast<std::uint8_t *>(occurrence.payload) -
                   ONDA_PRINT_RECORD_HEADER_SIZE,
        .capacity_bytes = static_cast<std::uint32_t>(recordBytes),
        .used_bytes = static_cast<std::uint32_t>(recordBytes),
        .record_count = 1U,
        .overflow_count = 0U,
    };
    std::size_t required{};
    if (onda_format_print_batch_into(instance_.get(), &batch, entry.text.data(),
                                     entry.text.size(), &required,
                                     nullptr) != 0 ||
        required >= entry.text.size()) {
      return false;
    }
    if (required > 0U && entry.text[required - 1U] == '\n')
      --required;
    entry.textBytes = static_cast<std::uint32_t>(required);
    const auto &site = logSites_[occurrence.site_index];
    entry.sourceFileBytes = copyText(entry.sourceFile, site.sourceFile);
    entry.lexicalOwnerBytes = copyText(entry.lexicalOwner, site.lexicalOwner);
    entry.line = site.line;
    return true;
  });
}

bool PreparedEngine::publishDelegate(
    const onda_delegate_occurrence_t &occurrence) noexcept {
  if (occurrence.delegate_index >= delegates_.size() ||
      (occurrence.payload_size_bytes != 0U && occurrence.payload == nullptr))
    return false;
  return publish(
      RuntimeLogKind::delegate,
      [this, &occurrence](RuntimeLogEntry &entry) noexcept {
        const auto &delegate = delegates_[occurrence.delegate_index];
        auto valid =
            appendText(entry, "delegate ") && appendText(entry, delegate.name);
        if (!delegate.plan.parameters.empty())
          valid = valid && appendText(entry, ": ");

        std::size_t cursor{};
        std::size_t displayed{};
        for (const auto &parameter : delegate.plan.parameters) {
          std::size_t dynamicCount{1U};
          if (parameter.dynamic) {
            if (cursor > occurrence.payload_size_bytes ||
                sizeof(std::uint32_t) >
                    occurrence.payload_size_bytes - cursor) {
              valid = false;
              break;
            }
            dynamicCount =
                readLittleEndian<std::uint32_t>(occurrence.payload + cursor);
            cursor += sizeof(std::uint32_t);
          }
          for (const auto &leaf : parameter.leaves) {
            if (displayed++ != 0U)
              valid = valid && appendText(entry, " ");
            valid =
                valid && appendText(entry, leaf.path) && appendText(entry, "=");
            const auto scalarBytes = payloadScalarBytes(leaf.scalar);
            if (dynamicCount >
                std::numeric_limits<std::size_t>::max() / leaf.fixedElements) {
              valid = false;
              break;
            }
            const auto count = dynamicCount * leaf.fixedElements;
            if (scalarBytes == 0U || cursor > occurrence.payload_size_bytes ||
                count >
                    (occurrence.payload_size_bytes - cursor) / scalarBytes) {
              valid = false;
              break;
            }
            const auto collection = parameter.dynamic || count != 1U;
            if (collection)
              valid = valid && appendText(entry, "[");
            for (std::size_t index = 0; valid && index < count; ++index) {
              if (index != 0U)
                valid = appendText(entry, ", ");
              valid = valid && appendPayloadScalar(entry, leaf.scalar,
                                                   occurrence.payload + cursor);
              cursor += scalarBytes;
            }
            if (collection)
              valid = valid && appendText(entry, "]");
          }
        }
        return valid && cursor == occurrence.payload_size_bytes;
      });
}

void PreparedEngine::collectExecutionOutput() noexcept {
  flushPendingLogs();
  onda_batch_cursor_t delegateCursor{};
  onda_batch_cursor_t printCursor{};
  onda_delegate_occurrence_t delegate{};
  onda_print_occurrence_t print{};
  auto hasDelegate = executionOutput_.delegate_batch != nullptr &&
                     onda_delegate_batch_next(&delegateBatch_, &delegateCursor,
                                              &delegate) == 1;
  auto hasPrint =
      executionOutput_.print_batch != nullptr &&
      onda_print_batch_next(&printBatch_, &printCursor, &print) == 1;
  std::uint64_t delegateDrops{};
  std::uint64_t printDrops{};
  while (hasDelegate || hasPrint) {
    if (!hasPrint || (hasDelegate && delegate.sequence < print.sequence)) {
      if (!publishDelegate(delegate))
        addSaturated(delegateDrops, 1U);
      hasDelegate = onda_delegate_batch_next(&delegateBatch_, &delegateCursor,
                                             &delegate) == 1;
    } else {
      if (!publishPrint(print))
        addSaturated(printDrops, 1U);
      hasPrint = onda_print_batch_next(&printBatch_, &printCursor, &print) == 1;
    }
  }
  addTransportDrop(RuntimeLogKind::delegate, delegateDrops);
  addTransportDrop(RuntimeLogKind::print, printDrops);
  addGeneratedOverflow(RuntimeLogKind::delegate, delegateBatch_.overflow_count);
  addGeneratedOverflow(RuntimeLogKind::print, printBatch_.overflow_count);
}

BuildResult
PreparedEngine::build(const std::filesystem::path &path, const Product product,
                      const double sampleRate, const int blockSize,
                      const std::span<const BufferFileBinding> bufferBindings,
                      const std::optional<ParameterValues> &initialParameters) {
  return build(compileFile(path, sampleRate, blockSize), product, sampleRate,
               blockSize, bufferBindings, path, initialParameters);
}

BuildResult
PreparedEngine::build(const ProjectImage &projectImage, const Product product,
                      const double sampleRate, const int blockSize,
                      const std::optional<ParameterValues> &initialParameters) {
  return build(compileProjectImage(projectImage, sampleRate, blockSize),
               product, sampleRate, blockSize, {}, {}, initialParameters);
}

BuildResult
PreparedEngine::build(CompileResult compiled, const Product product,
                      const double sampleRate, const int blockSize,
                      const std::span<const BufferFileBinding> bufferBindings,
                      const std::filesystem::path &diskEntry,
                      const std::optional<ParameterValues> &initialParameters) {
  BuildResult result;
  result.projectImage = compiled.projectImage;
  result.watchPaths = std::move(compiled.watchPaths);
  result.diagnostic = std::move(compiled.diagnostic);
  if (!compiled.program)
    return result;
  const auto hasProjectDefaults = result.projectImage.valid();

  Diagnostic diagnostic;
  const auto inputs = flattenedChannels(compiled.program.get(), true,
                                        pluginTotalInputChannels, diagnostic);
  const auto outputs = flattenedChannels(compiled.program.get(), false,
                                         pluginOutputChannels, diagnostic);
  if (inputs < 0 || outputs < 0) {
    result.diagnostic = std::move(diagnostic);
    return result;
  }
  const auto bufferCount = onda_buffer_count(compiled.program.get());
  if (bufferCount < 0) {
    result.diagnostic = error("Onda returned invalid buffer metadata");
    return result;
  }
  std::vector<BufferStorage> bufferStorage(
      static_cast<std::size_t>(bufferCount));
  std::vector<BufferMapping> bufferMappings;
  bufferMappings.reserve(static_cast<std::size_t>(bufferCount));
  std::vector<ProjectBufferAsset> projectBufferAssets;
  projectBufferAssets.reserve(static_cast<std::size_t>(bufferCount));
  std::string bufferError;
  for (int index = 0; index < bufferCount; ++index) {
    const auto *rawName = onda_buffer_name(compiled.program.get(), index);
    const auto *rawType = onda_buffer_type(compiled.program.get(), index);
    const auto rawKind =
        onda_buffer_channels_kind(compiled.program.get(), index);
    if (rawName == nullptr || rawType == nullptr ||
        onda_buffer_elem_type(compiled.program.get(), index) < 0 ||
        (rawKind != ONDA_BUFFER_CHANNELS_MONO &&
         rawKind != ONDA_BUFFER_CHANNELS_STATIC &&
         rawKind != ONDA_BUFFER_CHANNELS_DYNAMIC)) {
      result.diagnostic = error("Onda returned invalid buffer metadata");
      return result;
    }

    BufferMapping mapping;
    mapping.index = index;
    mapping.name = rawName;
    mapping.type = rawType;
    mapping.channelKind = rawKind == ONDA_BUFFER_CHANNELS_MONO
                              ? BufferChannelKind::mono
                              : (rawKind == ONDA_BUFFER_CHANNELS_STATIC
                                     ? BufferChannelKind::fixed
                                     : BufferChannelKind::dynamic);
    if (mapping.channelKind == BufferChannelKind::fixed) {
      mapping.fixedChannels =
          onda_buffer_channels_static(compiled.program.get(), index);
      if (mapping.fixedChannels <= 0) {
        result.diagnostic = error("Onda returned invalid buffer metadata");
        return result;
      }
    }

    const auto asset = std::ranges::find(compiled.projectBuffers, mapping.name,
                                         &ProjectBufferInfo::name);
    mapping.hasProjectDefault =
        hasProjectDefaults && asset != compiled.projectBuffers.end();
    const auto binding = std::ranges::find(bufferBindings, mapping.name,
                                           &BufferFileBinding::name);
    if (binding == bufferBindings.end() || binding->path.empty()) {
      if (!hasProjectDefaults) {
        if (bufferError.empty())
          bufferError = "Buffer '" + mapping.name + "' is not bound";
        bufferMappings.push_back(std::move(mapping));
        continue;
      }

      if (asset == compiled.projectBuffers.end()) {
        if (bufferError.empty())
          bufferError =
              "Buffer '" + mapping.name + "' is missing from the saved project";
      } else if (asset->frames > std::numeric_limits<int>::max() ||
                 asset->channels > std::numeric_limits<int>::max()) {
        if (bufferError.empty())
          bufferError = "Saved project buffer '" + mapping.name +
                        "' has unsupported dimensions";
      } else {
        mapping.loadedFrames = static_cast<int>(asset->frames);
        mapping.loadedChannels = static_cast<int>(asset->channels);
        mapping.loadedSampleRate = asset->sampleRate;
      }
      bufferMappings.push_back(std::move(mapping));
      continue;
    }

    result.watchPaths.push_back(binding->path);
    if (onda_buffer_may_write(compiled.program.get(), index) != 0) {
      if (bufferError.empty()) {
        bufferError = "Buffer '" + mapping.name +
                      "' is writable; audio-file bindings must be read-only "
                      "so the project can be saved and restored";
      }
      bufferMappings.push_back(std::move(mapping));
      continue;
    }
    if (onda_buffer_elem_type(compiled.program.get(), index) !=
        ONDA_PRIMITIVE_F32) {
      if (bufferError.empty()) {
        bufferError = "Audio-file bindings require an f32 Onda buffer, but '" +
                      mapping.name + "' is " + mapping.type;
      }
      bufferMappings.push_back(std::move(mapping));
      continue;
    }

    auto decoded = decodeAudioFile(binding->path);
    if (!decoded.audio) {
      if (bufferError.empty())
        bufferError = std::move(decoded.error);
      bufferMappings.push_back(std::move(mapping));
      continue;
    }
    const auto expectedChannels =
        mapping.channelKind == BufferChannelKind::mono
            ? 1
            : (mapping.channelKind == BufferChannelKind::fixed
                   ? mapping.fixedChannels
                   : decoded.audio->channels);
    if (decoded.audio->channels != expectedChannels) {
      if (bufferError.empty()) {
        bufferError = "Audio file for buffer '" + mapping.name + "' has " +
                      std::to_string(decoded.audio->channels) +
                      " channels; expected " + std::to_string(expectedChannels);
      }
      bufferMappings.push_back(std::move(mapping));
      continue;
    }

    auto &storage = bufferStorage[static_cast<std::size_t>(index)];
    storage.samples = std::move(decoded.audio->interleavedSamples);
    storage.frames = decoded.audio->frames;
    storage.channels = decoded.audio->channels;
    storage.sampleRate = decoded.audio->sampleRate;
    mapping.loadedPath = binding->path;
    mapping.loadedFrames = storage.frames;
    mapping.loadedChannels = storage.channels;
    mapping.loadedSampleRate = storage.sampleRate;
    Diagnostic assetDiagnostic;
    auto encoded =
        encodeF32BufferAsset(storage.samples, storage.frames, storage.channels,
                             storage.sampleRate, assetDiagnostic);
    if (encoded.empty()) {
      if (bufferError.empty())
        bufferError = std::move(assetDiagnostic.message);
    } else {
      projectBufferAssets.push_back(
          {.name = mapping.name, .encodedBytes = std::move(encoded)});
    }
    bufferMappings.push_back(std::move(mapping));
  }
  if (!bufferError.empty()) {
    result.buffers = std::move(bufferMappings);
    result.diagnostic = error(std::move(bufferError));
    return result;
  }

  OndaDiagnostic rawDiagnostic;
  InstanceHandle instance{onda_instance_create(
      compiled.program.get(), inputs, outputs, rawDiagnostic.outParameter())};
  result.diagnostic = rawDiagnostic.copy();
  if (!instance) {
    if (result.diagnostic.empty())
      result.diagnostic = error("Failed to create the Onda runtime instance");
    return result;
  }

  const auto slabSize = [blockSize](const int channels) {
    return static_cast<std::size_t>(channels) *
           static_cast<std::size_t>(blockSize);
  };
  const auto inputSamples = slabSize(inputs);
  const auto outputSamples = slabSize(outputs);
  const auto maximumSamples = std::vector<float>{}.max_size();
  if (inputSamples > maximumSamples || outputSamples > maximumSamples) {
    result.diagnostic = error("The prepared audio slab is too large");
    return result;
  }
  std::vector<float> inputSlab(inputSamples);
  std::vector<float> outputSlab(outputSamples);
  auto engine = std::unique_ptr<PreparedEngine>(new PreparedEngine(
      sampleRate, blockSize, std::move(compiled.program), std::move(instance),
      inputs, outputs, std::move(inputSlab), std::move(outputSlab),
      std::move(bufferStorage), std::move(bufferMappings)));

  if (!engine->prepareInputDefaults(result.diagnostic) ||
      !engine->prepareRuntimeOutput(result.diagnostic))
    return result;
  if (!bindAudio(engine->instance_.get(), engine->program_.get(),
                 engine->inputSlab_, true, blockSize, result.diagnostic) ||
      !bindAudio(engine->instance_.get(), engine->program_.get(),
                 engine->outputSlab_, false, blockSize, result.diagnostic)) {
    return result;
  }
  for (int index = 0; index < bufferCount; ++index) {
    auto &storage = engine->bufferStorage_[static_cast<std::size_t>(index)];
    if (!storage.samples.empty()) {
      if (onda_bind_buffer(engine->instance_.get(), index,
                           storage.samples.data(), storage.frames,
                           storage.channels, storage.sampleRate,
                           ONDA_PRIMITIVE_F32) != 0) {
        result.diagnostic = error(
            "Failed to bind audio file for Onda buffer '" +
            engine->bufferMappings_[static_cast<std::size_t>(index)].name +
            "'");
        return result;
      }
    }
  }
  const auto parameterCount = onda_param_count(engine->program_.get());
  if (parameterCount < 0) {
    result.diagnostic = error("Onda returned invalid parameter metadata");
    return result;
  }
  for (int index = 0;
       index < parameterCount && engine->parameterMappingCount_ < slotCount;
       ++index) {
    auto mapping = parameterMapping(engine->program_.get(), index);
    if (!mapping)
      continue;
    const auto slot = engine->parameterMappingCount_++;
    const auto primitive = onda_param_elem_type(engine->program_.get(), index);
    if (!mapping->stepCount &&
        (primitive == ONDA_PRIMITIVE_F32 || primitive == ONDA_PRIMITIVE_F64))
      engine->smoothedParameterTypes_[slot] = primitive;
    engine->parameterDomains_[slot] = mapping->scalarDomain();
    engine->parameterHostDomains_[slot] = mapping->hostDomain();
    engine->resolvedParameterValues_[slot] = mapping->parameterDefault();
    engine->parameterMappings_[slot] = std::move(*mapping);
  }

  if (initialParameters) {
    for (std::size_t index = 0; index < engine->parameterMappingCount_; ++index)
      engine->applyParameter(index, (*initialParameters)[index]);
  }
  if (!engine->initialize()) {
    result.diagnostic = error("Failed to initialize the Onda runtime instance");
    return result;
  }
  if (onda_prepare_unchecked_process(engine->instance_.get()) != 0) {
    if (result.diagnostic.empty())
      result.diagnostic = error("Failed to prepare unchecked Onda processing");
    return result;
  }

  for (std::size_t index = 0; index < expectedMidiEvents.size(); ++index) {
    if (!validateEvent(engine->program_.get(), expectedMidiEvents[index],
                       product, engine->midiEvents_[index],
                       result.diagnostic)) {
      return result;
    }
  }
  for (std::size_t index = 0; index < expectedHostContextEvents.size();
       ++index) {
    if (!validateEvent(engine->program_.get(), expectedHostContextEvents[index],
                       product, engine->hostContextEvents_[index],
                       result.diagnostic)) {
      return result;
    }
    if (index < static_cast<std::size_t>(HostContextKind::renderMode) &&
        engine->hostContextEvents_[index].index >= 0) {
      engine->needsPositionInfo_ = true;
    }
  }
  if (!collectVisibleEvents(engine->program_.get(), engine->eventMappings_,
                            result.diagnostic)) {
    return result;
  }

  if (result.projectImage.valid() && !projectBufferAssets.empty()) {
    result.projectImage = withProjectBufferOverrides(
        result.projectImage, projectBufferAssets, result.diagnostic);
    if (!result.projectImage.valid())
      return result;
  } else if (!result.projectImage.valid()) {
    result.projectImage =
        captureProjectImage(diskEntry, compiled.manifest.get(),
                            projectBufferAssets, result.diagnostic);
    if (!result.projectImage.valid())
      return result;
  }

  result.diagnostic = {};
  result.engine = std::move(engine);
  return result;
}

void PreparedEngine::applyParameter(const std::size_t slot,
                                    const ScalarValue &value) noexcept {
  const auto normalized = value.normalized;
  const auto exact = value.defaultPlain(parameterDomains_[slot]);
  resolvedParameterValues_[slot] = value;
  if (parametersStarted_ && !parametersSettleRequested_ &&
      normalized == parameterTargets_[slot] &&
      exact == parameterPlainDefaults_[slot] && !parameterRamps_[slot].active())
    return;
  parameterTargets_[slot] = normalized;
  parameterPlainDefaults_[slot] = exact;
  const auto plain =
      exact ? *exact
            : ScalarPlain{onda_processor_param_normalized_to_plain(
                  &parameterHostDomains_[slot], normalized)};
  parameterRamps_[slot].reset(writeParameterPlain(slot, plain));
}

double PreparedEngine::writeParameterPlain(const std::size_t slot,
                                           const ScalarPlain &value) noexcept {
  const auto index = parameterMappings_[slot].parameterIndex;
  const auto write = [this, index]<typename T>(const T scalar) noexcept {
    static_cast<void>(onda_set_param_by_index(instance_.get(), index, &scalar,
                                              sizeof(scalar)));
    return static_cast<double>(scalar);
  };
  return std::visit(
      [&](const auto scalar) noexcept {
        switch (parameterDomains_[slot].type) {
        case ScalarDomain::Type::f32:
          return write(static_cast<float>(scalar));
        case ScalarDomain::Type::f64:
          return write(static_cast<double>(scalar));
        case ScalarDomain::Type::i32:
          return write(static_cast<std::int32_t>(scalar));
        case ScalarDomain::Type::i64:
          return write(static_cast<std::int64_t>(scalar));
        case ScalarDomain::Type::boolean:
          return write(static_cast<double>(scalar) >= 0.5);
        }
        return static_cast<double>(scalar);
      },
      value);
}

template <PreparedEngine::ValueBank Bank, std::size_t Count>
ScalarValue PreparedEngine::resolveValue(
    const std::size_t index, const ParameterValue::Target target,
    const ParameterBank<Count> *values) const noexcept {
  constexpr auto isControl = Bank == ValueBank::controls;
  static_assert(Count == (isControl ? controlCount : slotCount));
  auto &resolved = isControl ? resolvedControlValues_[index]
                             : resolvedParameterValues_[index];
  if (target.editor && target.defaulted &&
      (!isControl || index < controlMappings_.size())) {
    auto &cached = isControl ? controlResetDefaults_[index]
                             : parameterResetDefaults_[index];
    if (cached.stamp != target.stamp && values != nullptr) {
      if (const auto value = values->defaultValue(index, target.stamp)) {
        cached.value = *value;
        cached.stamp = target.stamp;
      }
    }
    // A concurrent publication keeps the fallback and retries next time.
    // Only complete Reset snapshots are cached; seeding and automation may
    // overwrite the fallback without changing this immutable stamped value.
    if (cached.stamp == target.stamp)
      resolved = cached.value;
    return resolved;
  }
  if (parameterSeed_ &&
      index <
          (isControl ? parameterSeed_->controlCount : parameterSeed_->count) &&
      parameterSeed_->applied.load(std::memory_order_acquire) &&
      target.stamp == (isControl ? parameterSeed_->controlStamps.after[index]
                                 : parameterSeed_->parameterStamps.after[index])
                          .load(std::memory_order_acquire))
    return resolved = isControl ? parameterSeed_->controls[index]
                                : parameterSeed_->values[index];
  auto checkpointStamp =
      checkpointValues_
          ? (isControl ? checkpointValues_->controlStamps[index]
                       : checkpointValues_->parameterStamps[index])
          : 0U;
  if constexpr (isControl) {
    if (target.defaulted && initialControlStamps_ != nullptr &&
        checkpointStamp == ParameterValue::initialStamp) {
      const auto initialized =
          (*initialControlStamps_)[index].load(std::memory_order_acquire);
      if (initialized != 0U)
        checkpointStamp = initialized;
    }
  }
  if (checkpointValues_ && checkpointStamp == target.stamp)
    return resolved = isControl ? checkpointValues_->controls[index]
                                : checkpointValues_->parameters[index];
  return resolved = ScalarValue{target.value};
}

template <PreparedEngine::ValueBank Bank, std::size_t Count>
ParameterValue::Target PreparedEngine::resolveTarget(
    const std::size_t index,
    const ParameterBank<Count> &values) const noexcept {
  auto target = values.target(index);
  const auto value = resolveValue<Bank>(index, target, &values);
  target.value = value.normalized;
  target.defaulted = value.scalarDefault.has_value();
  return target;
}

ScalarValue
PreparedEngine::parameterValue(const std::size_t slot,
                               const ParameterValue::Target target,
                               const ParameterState &slots) const noexcept {
  return resolveValue<ValueBank::parameters>(slot, target, &slots);
}

ParameterValue::Target
PreparedEngine::parameterTarget(const std::size_t slot,
                                const ParameterState &values) const noexcept {
  return resolveTarget<ValueBank::parameters>(slot, values);
}

void PreparedEngine::applyParameters(const ParameterState &slots) noexcept {
  const auto useDefaults =
      hostCallbackUsesDefaults_ || parameterDefaultsPending();
  for (std::size_t index = 0; index < parameterMappingCount_; ++index) {
    applyParameter(index,
                   useDefaults && index < parameterSeed_->count
                       ? parameterSeed_->values[index]
                       : parameterValue(index, slots.target(index), slots));
  }
}

void PreparedEngine::beginHostCallback(const int frames,
                                       ParameterAutomation *automation,
                                       const ControlState *controls,
                                       ControlAutomation *controlAutomation,
                                       const bool useDefaults) noexcept {
  hostCallbackFrames_ = frames < 0 ? blockSize_ : frames;
  // Hold one seeding decision for the whole callback, including acknowledgment
  // during processing. Deferred curves start with the following callback.
  hostCallbackUsesDefaults_ =
      parameterDefaultsPending() || (useDefaults && parameterSeed_ != nullptr);
  hostAutomation_ = !hostCallbackUsesDefaults_ && automation != nullptr &&
                            automation->boundLanes() != 0U
                        ? automation
                        : nullptr;
  if (hostAutomation_ != nullptr) {
    const auto mappedLanes = (std::uint64_t{1} << parameterMappingCount_) - 1U;
    detail::forEachAutomationLane(
        hostAutomation_->boundLanes() & mappedLanes,
        [this](const std::size_t slot) noexcept {
          if (hostAutomation_->active(slot))
            hostAutomation_->start(slot, smoothedParameterTypes_[slot] >= 0);
        });
  }
  controlState_ = controls;
  controlAutomation_ = !hostCallbackUsesDefaults_ &&
                               controlAutomation != nullptr &&
                               controlAutomation->boundLanes() != 0U
                           ? controlAutomation
                           : nullptr;
  controlInputsPrepared_ = false;
  // Event-only processing can prepare a boundary without rendering samples.
  // Refresh it for this callback's targets and actual first chunk length.
  if (logicalFrame_ == 0)
    blockStarted_ = false;
}

bool PreparedEngine::setParameterSmoothingSeconds(
    const double seconds) noexcept {
  const auto count = parameterSmoothingSamples(seconds, sampleRate_);
  if (!count)
    return false;
  const auto samples =
      *count > static_cast<std::uint64_t>(blockSize_) ? *count : 0U;
  if (samples != smoothingSamples_) {
    smoothingSamples_ = samples;
    for (auto &ramp : parameterRamps_)
      ramp.setDuration(samples);
    if (samples == 0U)
      parametersSettleRequested_ = true;
  }
  return true;
}

void PreparedEngine::settleParameters() noexcept {
  parametersSettleRequested_ = true;
}

bool PreparedEngine::setInputControlSmoothingSeconds(
    const double seconds) noexcept {
  const auto samples = parameterSmoothingSamples(seconds, sampleRate_);
  if (!samples)
    return false;
  if (*samples != controlSmoothingSamples_) {
    controlSmoothingSamples_ = *samples;
    for (auto &ramp : controlRamps_)
      ramp.setDuration(*samples);
  }
  return true;
}

bool PreparedEngine::inputControlRampPending(
    const std::size_t control, const double normalized,
    const double seconds) const noexcept {
  return control < controlMappings_.size() && controlValuesStarted_[control] &&
         seconds > 0.0 &&
         (controlRamps_[control].active() || controlUsesDefault_[control] ||
          controlNormalizedTargets_[control] != normalized);
}

bool PreparedEngine::editorRampPending(const std::size_t slot,
                                       const double normalized,
                                       const double seconds) const noexcept {
  return slot < parameterMappingCount_ && parametersStarted_ &&
         !parametersSettleRequested_ && smoothedParameterTypes_[slot] >= 0 &&
         seconds * sampleRate_ > static_cast<double>(blockSize_) &&
         (parameterRamps_[slot].active() || parameterPlainDefaults_[slot] ||
          parameterTargets_[slot] != normalized);
}

void PreparedEngine::updateParameters(const ParameterState &slots,
                                      const int frames,
                                      const int offset) noexcept {
  const auto useDefaults =
      hostCallbackUsesDefaults_ || parameterDefaultsPending();
  for (std::size_t slot = 0; slot < parameterMappingCount_; ++slot) {
    const auto requested = slots.target(slot);
    if (!useDefaults && hostAutomation_ != nullptr &&
        hostAutomation_->active(slot, requested.stamp)) {
      applyParameter(slot, hostAutomation_->valueAt(slot, offset));
      continue;
    }
    const auto value = useDefaults && slot < parameterSeed_->count
                           ? parameterSeed_->values[slot]
                           : parameterValue(slot, requested, slots);
    const auto normalized = value.normalized;
    const auto type = smoothedParameterTypes_[slot];
    if (!parametersStarted_ || parametersSettleRequested_ ||
        smoothingSamples_ == 0U || type < 0 || useDefaults ||
        !requested.editor || value.scalarDefault) {
      applyParameter(slot, value);
      continue;
    }
    auto &ramp = parameterRamps_[slot];
    if (normalized != parameterTargets_[slot] ||
        parameterPlainDefaults_[slot]) {
      auto target = onda_processor_param_normalized_to_plain(
          &parameterHostDomains_[slot], normalized);
      if (type == ONDA_PRIMITIVE_F32)
        target = static_cast<double>(static_cast<float>(target));
      ramp.setTarget(target);
      parameterTargets_[slot] = normalized;
      parameterPlainDefaults_[slot].reset();
    }
    if (ramp.active())
      static_cast<void>(writeParameterPlain(
          slot, ramp.advance(static_cast<std::uint64_t>(frames),
                             type == ONDA_PRIMITIVE_F32)));
  }
  parametersSettleRequested_ = false;
}

bool PreparedEngine::ensureBlockStarted(const ParameterState &slots,
                                        const HostContext &hostContext,
                                        const int hostCallbackOffset) noexcept {
  if (logicalFrame_ == 0 && !blockStarted_) {
    const auto frames =
        hostCallbackFrames_ >= 0
            ? std::clamp(hostCallbackFrames_ - hostCallbackOffset, 0,
                         blockSize_)
            : blockSize_;
    updateParameters(slots, frames, hostCallbackOffset);
    if (!dispatchHostContext(hostContext, hostCallbackOffset))
      return false;
    blockStarted_ = true;
  }
  return true;
}

ControlValue
PreparedEngine::controlValue(const std::size_t control,
                             const ParameterValue::Target target,
                             const ControlState *values) const noexcept {
  return resolveValue<ValueBank::controls>(control, target, values);
}

ParameterValue::Target
PreparedEngine::controlTarget(const std::size_t control,
                              const ControlState &values) const noexcept {
  return resolveTarget<ValueBank::controls>(control, values);
}

std::uint64_t PreparedEngine::initializeControlDefaults(
    ControlState &controls, const std::uint64_t retainedLanes) noexcept {
  if (checkpointValues_ == nullptr || initialControlStamps_ == nullptr)
    return 0U;
  std::uint64_t initialized{};
  for (auto index = parameterSeed_ ? parameterSeed_->controlCount
                                   : std::size_t{0};
       index < controlMappings_.size(); ++index) {
    const auto &value = checkpointValues_->controls[index];
    const auto lane = std::uint64_t{1} << index;
    if ((retainedLanes & lane) == 0U && value.scalarDefault &&
        checkpointValues_->controlStamps[index] ==
            ParameterValue::initialStamp &&
        controls.initializeDefault(index, static_cast<float>(value.normalized),
                                   (*initialControlStamps_)[index]) != 0U)
      initialized |= lane;
  }
  return initialized;
}

void PreparedEngine::prepareControlInputs(float *const *hostInputs) noexcept {
  const auto useDefaults =
      hostCallbackUsesDefaults_ || parameterDefaultsPending();
  for (std::size_t control = 0; control < controlMappings_.size(); ++control) {
    const auto channel =
        static_cast<std::size_t>(pluginInputChannels) + control;
    if (hostInputs != nullptr && hostInputs[channel] != nullptr) {
      controlValuesStarted_[control] = false;
      continue;
    }
    if (controlAutomation_ != nullptr && controlAutomation_->active(control)) {
      // Preserve the last rendered value if a newer editor write cancels this
      // curve before rendering. Only rendered automation commits ramp state.
      controlAutomation_->start(control, true);
      continue;
    }
    ControlValue value;
    ParameterValue::Target target;
    if (useDefaults && control < parameterSeed_->controlCount) {
      value = parameterSeed_->controls[control];
    } else if (controlState_ != nullptr && !useDefaults) {
      target = controlState_->target(control);
      value = controlValue(control, target, controlState_);
    } else {
      controlRamps_[control].reset(inputDefaults_[channel]);
      continue;
    }
    updateControlTarget(control, target, value, useDefaults);
  }
  controlInputsPrepared_ = true;
}

void PreparedEngine::updateControlTarget(const std::size_t control,
                                         const ParameterValue::Target target,
                                         const ControlValue &value,
                                         const bool useDefaults) noexcept {
  auto &ramp = controlRamps_[control];
  const auto plain =
      value.plain(controlMinimum_[control], controlMaximum_[control]);
  if (!controlValuesStarted_[control] || useDefaults || !target.editor ||
      value.scalarDefault || controlSmoothingSamples_ == 0U) {
    ramp.reset(plain);
  } else {
    ramp.setTarget(plain);
  }
  controlUsesDefault_[control] = value.scalarDefault.has_value();
  controlNormalizedTargets_[control] = value.normalized;
}

bool PreparedEngine::processSegment(float *const *hostInputs,
                                    float *const *hostOutputs,
                                    const int callbackOffset, const int frames,
                                    const ParameterState &slots,
                                    const HostContext &hostContext,
                                    const int hostCallbackOffset) noexcept {
  if (!controlInputsPrepared_)
    prepareControlInputs(hostInputs);
  for (int channel = 0; channel < inputChannels_; ++channel) {
    auto *destination =
        inputSlab_.data() + channel * blockSize_ + logicalFrame_;
    if (hostInputs != nullptr && hostInputs[channel] != nullptr) {
      std::copy_n(hostInputs[channel] + callbackOffset, frames, destination);
    } else if (channel >= pluginInputChannels) {
      const auto control =
          static_cast<std::size_t>(channel - pluginInputChannels);
      if (controlAutomation_ != nullptr &&
          controlAutomation_->active(control)) {
        const auto target = controlState_ != nullptr
                                ? controlState_->target(control)
                                : ParameterValue::Target{};
        if (controlState_ == nullptr ||
            controlAutomation_->active(control, target.stamp)) {
          controlAutomation_->render(
              control, destination, hostCallbackOffset + callbackOffset, frames,
              controlMinimum_[control], controlMaximum_[control]);
          controlRamps_[control].reset(destination[frames - 1]);
          controlNormalizedTargets_[control] = target.value;
          controlUsesDefault_[control] = false;
          controlValuesStarted_[control] = true;
        } else {
          updateControlTarget(control, target,
                              controlValue(control, target, controlState_),
                              false);
          controlRamps_[control].render(destination, frames);
          controlValuesStarted_[control] = true;
        }
      } else {
        controlRamps_[control].render(destination, frames);
        controlValuesStarted_[control] = true;
      }
    } else {
      std::fill_n(destination, frames,
                  inputDefaults_[static_cast<std::size_t>(channel)]);
    }
  }

  for (int channel = 0; channel < pluginOutputChannels; ++channel)
    std::fill_n(hostOutputs[channel] + callbackOffset, frames, 0.0F);
  for (int channel = 0; channel < outputChannels_; ++channel) {
    std::fill_n(outputSlab_.data() + channel * blockSize_ + logicalFrame_,
                frames, 0.0F);
  }

  int flags = 0;
  if (logicalFrame_ == 0) {
    flags |= ONDA_PROCESS_BEGIN_BLOCK;
    if (!ensureBlockStarted(slots, hostContext,
                            hostCallbackOffset + callbackOffset)) {
      return false;
    }
  }
  // A callback owns its chunks. MIDI splits keep the current chunk open;
  // its final real segment closes it, including a short callback tail.
  const auto endBlock =
      logicalFrame_ + frames == blockSize_ ||
      (hostCallbackFrames_ >= 0 &&
       hostCallbackOffset + callbackOffset + frames == hostCallbackFrames_);
  if (endBlock)
    flags |= ONDA_PROCESS_END_BLOCK;

  const auto status = onda_process_unchecked_segment(
      instance_.get(), logicalFrame_, frames, flags, &executionOutput_);
  collectExecutionOutput();
  if (status != 0) {
    return false;
  }

  // Events can prestart a block without rendering any audio. Only rendered
  // samples complete startup and enable smoothing of subsequent targets.
  parametersStarted_ = true;
  for (int channel = 0; channel < outputChannels_; ++channel) {
    std::copy_n(outputSlab_.data() + channel * blockSize_ + logicalFrame_,
                frames, hostOutputs[channel] + callbackOffset);
  }
  logicalFrame_ += frames;
  if (endBlock) {
    logicalFrame_ = 0;
    blockStarted_ = false;
  }
  return true;
}

bool PreparedEngine::handlesMidi(const MidiKind kind) const noexcept {
  const auto index = static_cast<std::size_t>(kind);
  return index < midiEvents_.size() && midiEvents_[index].index >= 0;
}

bool PreparedEngine::handlesHostContext(
    const HostContextKind kind) const noexcept {
  const auto index = static_cast<std::size_t>(kind);
  return index < hostContextEvents_.size() &&
         hostContextEvents_[index].index >= 0;
}

bool PreparedEngine::dispatch(const MidiEvent &event) noexcept {
  const auto kind = static_cast<std::size_t>(event.kind);
  if (kind >= midiEvents_.size())
    return false;
  const auto &binding = midiEvents_[kind];
  switch (event.kind) {
  case MidiKind::noteOn:
  case MidiKind::noteOff:
    return trigger(binding, std::int32_t{-1}, event.channel,
                   event.keyOrController, event.value);
  case MidiKind::polyPressure:
    return trigger(binding, event.channel, event.keyOrController, event.value);
  case MidiKind::pitchBend:
  case MidiKind::channelPressure:
    return trigger(binding, event.channel, event.value);
  case MidiKind::controlChange:
    return trigger(binding, event.channel, event.keyOrController, event.value);
  case MidiKind::programChange:
    return trigger(binding, event.channel, event.keyOrController);
  case MidiKind::count:
    return false;
  }
  return false;
}

bool PreparedEngine::dispatchHostContext(
    const HostContext &hostContext, const int hostCallbackOffset) noexcept {
  const auto positionOffset =
      hostContext.timelinePlaying ? hostCallbackOffset : 0;
  const auto binding = [this](const HostContextKind event) -> const auto & {
    return hostContextEvents_[static_cast<std::size_t>(event)];
  };

  if (handlesHostContext(HostContextKind::transport) && hostContext.transport &&
      !trigger(binding(HostContextKind::transport),
               hostContext.transport->playing, hostContext.transport->recording,
               hostContext.transport->looping)) {
    return false;
  }

  if (handlesHostContext(HostContextKind::samplePosition) &&
      hostContext.samplePosition) {
    const auto sample = *hostContext.samplePosition;
    if (positionOffset <= 0 ||
        sample <= std::numeric_limits<std::int64_t>::max() - positionOffset) {
      const auto projected =
          sample + static_cast<std::int64_t>(std::max(positionOffset, 0));
      if (!trigger(binding(HostContextKind::samplePosition), projected)) {
        return false;
      }
    }
  }

  const auto needsSecondsOffset =
      positionOffset != 0 &&
      ((handlesHostContext(HostContextKind::timePosition) &&
        hostContext.timePosition.has_value()) ||
       (handlesHostContext(HostContextKind::musicalPosition) &&
        hostContext.musicalPosition.has_value()));
  const auto secondsOffset =
      needsSecondsOffset ? static_cast<double>(positionOffset) / sampleRate_
                         : 0.0;
  if (handlesHostContext(HostContextKind::timePosition) &&
      hostContext.timePosition) {
    const auto projected = *hostContext.timePosition + secondsOffset;
    if (std::isfinite(projected) &&
        !trigger(binding(HostContextKind::timePosition), projected)) {
      return false;
    }
  }

  if (handlesHostContext(HostContextKind::tempo) && hostContext.tempo &&
      std::isfinite(*hostContext.tempo) && *hostContext.tempo > 0.0 &&
      !trigger(binding(HostContextKind::tempo), *hostContext.tempo)) {
    return false;
  }

  if (handlesHostContext(HostContextKind::musicalPosition) &&
      hostContext.musicalPosition) {
    auto projected = *hostContext.musicalPosition;
    if (positionOffset != 0) {
      if (!hostContext.tempo || !std::isfinite(*hostContext.tempo) ||
          *hostContext.tempo <= 0.0) {
        projected = std::numeric_limits<double>::quiet_NaN();
      } else {
        projected += secondsOffset * *hostContext.tempo / 60.0;
      }
    }
    if (std::isfinite(projected) &&
        !trigger(binding(HostContextKind::musicalPosition), projected)) {
      return false;
    }
  }

  if (handlesHostContext(HostContextKind::barPosition) &&
      hostContext.barPosition && std::isfinite(*hostContext.barPosition) &&
      !trigger(binding(HostContextKind::barPosition),
               *hostContext.barPosition)) {
    return false;
  }

  if (handlesHostContext(HostContextKind::timeSignature) &&
      hostContext.timeSignature && hostContext.timeSignature->numerator > 0 &&
      hostContext.timeSignature->denominator > 0 &&
      !trigger(binding(HostContextKind::timeSignature),
               hostContext.timeSignature->numerator,
               hostContext.timeSignature->denominator)) {
    return false;
  }

  if (handlesHostContext(HostContextKind::loopRegion) &&
      hostContext.loopRegion &&
      std::isfinite(hostContext.loopRegion->startQuarterNote) &&
      std::isfinite(hostContext.loopRegion->endQuarterNote) &&
      !trigger(binding(HostContextKind::loopRegion),
               hostContext.loopRegion->startQuarterNote,
               hostContext.loopRegion->endQuarterNote)) {
    return false;
  }

  return !handlesHostContext(HostContextKind::renderMode) ||
         trigger(binding(HostContextKind::renderMode), hostContext.realtime);
}

bool PreparedEngine::process(float *const *hostInputs,
                             float *const *hostOutputs, const int frames,
                             const std::span<const MidiEvent> midi,
                             const ParameterState &slots,
                             const HostContext &hostContext,
                             const int hostCallbackOffset) noexcept {
  if (frames < 0 || hostCallbackOffset < 0)
    return false;

  int position = 0;
  std::size_t eventIndex = 0;
  while (position < frames) {
    while (eventIndex < midi.size() &&
           midi[eventIndex].sampleOffset <=
               static_cast<std::uint32_t>(position)) {
      if (!ensureBlockStarted(slots, hostContext,
                              hostCallbackOffset + position)) {
        return false;
      }
      if (!dispatch(midi[eventIndex]))
        return false;
      ++eventIndex;
    }

    auto next = frames;
    if (eventIndex < midi.size()) {
      next = std::min(next, static_cast<int>(std::min<std::uint32_t>(
                                midi[eventIndex].sampleOffset,
                                static_cast<std::uint32_t>(frames))));
    }
    next = std::min(next, position + std::min(frames - position,
                                              blockSize_ - logicalFrame_));
    if (next == position)
      continue;
    if (!processSegment(hostInputs, hostOutputs, position, next - position,
                        slots, hostContext, hostCallbackOffset)) {
      return false;
    }
    position = next;
  }

  while (eventIndex < midi.size() &&
         midi[eventIndex].sampleOffset <= static_cast<std::uint32_t>(frames)) {
    if (!ensureBlockStarted(slots, hostContext, hostCallbackOffset + frames)) {
      return false;
    }
    if (!dispatch(midi[eventIndex]))
      return false;
    ++eventIndex;
  }
  return true;
}

EventTriggerResult PreparedEngine::triggerEvent(
    const int index, const std::span<const std::byte> payload,
    const ParameterState &slots, const HostContext &hostContext) noexcept {
  if (index < 0 ||
      payload.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      !ensureBlockStarted(slots, hostContext, 0)) {
    return EventTriggerResult::runtimeFailure;
  }
  const auto status = onda_trigger_event_by_index_unchecked(
      instance_.get(), index, payload.data(), static_cast<int>(payload.size()),
      &executionOutput_);
  collectExecutionOutput();
  if (status == ONDA_EXECUTION_OK)
    return EventTriggerResult::success;
  if (status == ONDA_EXECUTION_INPUT_REJECTED)
    return EventTriggerResult::inputRejected;
  return EventTriggerResult::runtimeFailure;
}

bool PreparedEngine::reset(const ParameterState &slots) noexcept {
  applyParameters(slots);
  parametersStarted_ = false;
  controlValuesStarted_.fill(false);
  parametersSettleRequested_ = false;
  logicalFrame_ = 0;
  blockStarted_ = false;
  const auto status = onda_init_unchecked(
      instance_.get(), ONDA_INIT_PRESERVE_PINNED, &executionOutput_);
  collectExecutionOutput();
  return status == 0 && onda_prepare_unchecked_process(instance_.get()) == 0;
}

} // namespace onda::plugin
