#include "Engine.h"

#include "AudioFile.h"
#include "Primitive.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace onda::plugin {
namespace {

static_assert(sizeof(float) == 4U);
static_assert(sizeof(double) == 8U);
static_assert(sizeof(std::int32_t) == 4U);
static_assert(sizeof(std::int64_t) == 8U);

constexpr std::size_t runtimeBatchCapacity = 64U * 1024U;
constexpr std::size_t maximumPendingLogEntries = runtimeLogQueueCapacity - 1U;

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
  const auto primitive = onda_param_elem_type(program, index);
  const auto type = primitiveName(primitive);
  const auto unit = parameterUnit(program, index);
  if (type.empty() || !unit)
    return std::nullopt;

  const auto plain = onda_param_normalized_to_plain(program, index, 0.5);
  if (!std::isfinite(plain) ||
      !std::isfinite(onda_param_plain_to_normalized(program, index, plain))) {
    return std::nullopt;
  }

  ParameterMapping mapping;
  mapping.parameterIndex = index;
  mapping.type = type;
  mapping.unit = *unit;
  if (const auto *name = onda_param_name(program, index))
    mapping.name = name;

  mapping.defaultPlain = onda_param_default_f64(program, index);
  const auto normalizedDefault =
      onda_param_plain_to_normalized(program, index, mapping.defaultPlain);
  if (!std::isfinite(mapping.defaultPlain) ||
      !std::isfinite(normalizedDefault)) {
    return std::nullopt;
  }
  mapping.defaultNormalized = std::clamp(normalizedDefault, 0.0, 1.0);

  if (primitive == ONDA_PRIMITIVE_BOOL) {
    mapping.rangeMin = 0.0;
    mapping.rangeMax = 1.0;
    return mapping;
  }

  if (onda_param_has_range(program, index) != 1)
    return std::nullopt;
  mapping.rangeMin = onda_param_range_min_f64(program, index);
  mapping.rangeMax = onda_param_range_max_f64(program, index);
  if (!std::isfinite(mapping.rangeMin) || !std::isfinite(mapping.rangeMax) ||
      mapping.rangeMin >= mapping.rangeMax) {
    return std::nullopt;
  }

  const auto scale = onda_param_scale(program, index);
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
  }
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

#if defined(__APPLE__)
template <typename Value>
  requires std::is_floating_point_v<Value>
bool appendNumber(RuntimeLogEntry &entry, const Value value) noexcept {
  // Floating-point to_chars requires macOS 13.3. The null locale selects
  // the C locale, and max_digits10 preserves enough digits to round-trip.
  std::array<char, 64U> text{};
  const auto written = ::snprintf_l(text.data(), text.size(), nullptr, "%.*g",
                                    std::numeric_limits<Value>::max_digits10,
                                    static_cast<double>(value));
  return written > 0 && static_cast<std::size_t>(written) < text.size() &&
         appendText(entry, {text.data(), static_cast<std::size_t>(written)});
}
#endif

template <typename Unsigned>
Unsigned readLittleEndian(const std::uint8_t *bytes) noexcept {
  Unsigned value{};
  for (std::size_t index = 0; index < sizeof(Unsigned); ++index)
    value |= static_cast<Unsigned>(bytes[index]) << (index * 8U);
  return value;
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
      bufferMappings_(std::move(bufferMappings)) {}

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
                                        pluginInputChannels, diagnostic);
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

    const auto binding = std::ranges::find(bufferBindings, mapping.name,
                                           &BufferFileBinding::name);
    if (binding == bufferBindings.end() || binding->path.empty()) {
      if (!hasProjectDefaults) {
        if (bufferError.empty())
          bufferError = "Buffer '" + mapping.name + "' is not bound";
        bufferMappings.push_back(std::move(mapping));
        continue;
      }

      const auto asset = std::ranges::find(
          compiled.projectBuffers, mapping.name, &ProjectBufferInfo::name);
      if (asset == compiled.projectBuffers.end()) {
        if (bufferError.empty())
          bufferError =
              "Buffer '" + mapping.name + "' is missing from the saved project";
      } else if (asset->elementType != ONDA_PRIMITIVE_F32 ||
                 asset->frames > std::numeric_limits<int>::max() ||
                 asset->channels > std::numeric_limits<int>::max()) {
        if (bufferError.empty())
          bufferError = "Saved project buffer '" + mapping.name +
                        "' is not a supported f32 audio buffer";
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

  if (!engine->prepareRuntimeOutput(result.diagnostic))
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
    engine->parameterMappings_[engine->parameterMappingCount_++] =
        std::move(*mapping);
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
                                    const float value) noexcept {
  const auto normalized =
      std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.5F;
  static_cast<void>(onda_set_param_normalized(
      instance_.get(), parameterMappings_[slot].parameterIndex,
      static_cast<double>(normalized)));
  appliedParameters_[slot] = normalized;
}

float PreparedEngine::parameterValue(
    const std::size_t slot,
    const std::array<std::atomic<float> *, slotCount> &slots,
    const bool useDefaults) const noexcept {
  const auto value = useDefaults && slot < parameterSeed_->count
                         ? parameterSeed_->values[slot]
                         : slots[slot]->load(std::memory_order_relaxed);
  return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.5F;
}

void PreparedEngine::applyParameters(
    const std::array<std::atomic<float> *, slotCount> &slots) noexcept {
  const auto useDefaults = parameterSeed_ && !parameterSeed_->applied.load(
                                                 std::memory_order_acquire);
  for (std::size_t index = 0; index < parameterMappingCount_; ++index) {
    applyParameter(index, parameterValue(index, slots, useDefaults));
  }
}

bool PreparedEngine::beginHostCallback(
    const std::array<std::atomic<float> *, slotCount> &slots) noexcept {
  if (!blockStarted_)
    return true;

  const auto useDefaults = parameterSeed_ && !parameterSeed_->applied.load(
                                                 std::memory_order_acquire);
  bool changed = false;
  for (std::size_t index = 0; index < parameterMappingCount_; ++index)
    changed |= parameterValue(index, slots, useDefaults) !=
               appliedParameters_[index];
  if (!changed)
    return true;

  if (logicalFrame_ == 0) {
    blockStarted_ = false;
    return true;
  }

  // Parameters in sample work are captured by BEGIN_BLOCK. Complete the
  // current short block so this callback can begin with the new values.
  const auto status = onda_process_unchecked_segment(
      instance_.get(), logicalFrame_, 0, ONDA_PROCESS_END_BLOCK,
      &executionOutput_);
  collectExecutionOutput();
  if (status != 0)
    return false;
  logicalFrame_ = 0;
  blockStarted_ = false;
  return true;
}

bool PreparedEngine::ensureBlockStarted(
    const std::array<std::atomic<float> *, slotCount> &slots,
    const HostContext &hostContext, const int hostCallbackOffset) noexcept {
  if (logicalFrame_ == 0 && !blockStarted_) {
    applyParameters(slots);
    if (!dispatchHostContext(hostContext, hostCallbackOffset))
      return false;
    blockStarted_ = true;
  }
  return true;
}

bool PreparedEngine::processSegment(
    float *const *hostInputs, float *const *hostOutputs,
    const int callbackOffset, const int frames,
    const std::array<std::atomic<float> *, slotCount> &slots,
    const HostContext &hostContext, const int hostCallbackOffset) noexcept {
  for (int channel = 0; channel < inputChannels_; ++channel) {
    auto *destination =
        inputSlab_.data() + channel * blockSize_ + logicalFrame_;
    if (hostInputs != nullptr && hostInputs[channel] != nullptr) {
      std::copy_n(hostInputs[channel] + callbackOffset, frames, destination);
    } else {
      std::fill_n(destination, frames, 0.0F);
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
  if (logicalFrame_ + frames == blockSize_)
    flags |= ONDA_PROCESS_END_BLOCK;

  const auto status = onda_process_unchecked_segment(
      instance_.get(), logicalFrame_, frames, flags, &executionOutput_);
  collectExecutionOutput();
  if (status != 0) {
    return false;
  }

  for (int channel = 0; channel < outputChannels_; ++channel) {
    std::copy_n(outputSlab_.data() + channel * blockSize_ + logicalFrame_,
                frames, hostOutputs[channel] + callbackOffset);
  }
  logicalFrame_ += frames;
  if (logicalFrame_ == blockSize_) {
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

bool PreparedEngine::process(
    float *const *hostInputs, float *const *hostOutputs, const int frames,
    const std::span<const MidiEvent> midi,
    const std::array<std::atomic<float> *, slotCount> &slots,
    const HostContext &hostContext, const int hostCallbackOffset) noexcept {
  if (frames < 0 || frames > blockSize_ || hostCallbackOffset < 0)
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
    next = std::min(next, position + blockSize_ - logicalFrame_);
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
    const std::array<std::atomic<float> *, slotCount> &slots,
    const HostContext &hostContext) noexcept {
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

bool PreparedEngine::reset(
    const std::array<std::atomic<float> *, slotCount> &slots) noexcept {
  applyParameters(slots);
  logicalFrame_ = 0;
  blockStarted_ = false;
  const auto status = onda_init_unchecked(
      instance_.get(), ONDA_INIT_PRESERVE_PINNED, &executionOutput_);
  collectExecutionOutput();
  return status == 0 && onda_prepare_unchecked_process(instance_.get()) == 0;
}

} // namespace onda::plugin
