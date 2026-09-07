#include "RunViewHost.h"

#include "JucePath.h"
#include "Processor.h"

#include <OndaRunResources.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace onda::plugin {
namespace {

using Resource = juce::WebBrowserComponent::Resource;

Resource utf8Resource(const char *data, const int size, juce::String mime) {
  constexpr std::array<std::byte, 3U> byteOrderMark{
      std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}};
  Resource result;
  result.data.resize(byteOrderMark.size() + static_cast<std::size_t>(size));
  std::copy(byteOrderMark.begin(), byteOrderMark.end(), result.data.begin());
  std::memcpy(result.data.data() + byteOrderMark.size(), data,
              static_cast<std::size_t>(size));
  result.mimeType = std::move(mime);
  return result;
}

juce::var object() { return juce::var{new juce::DynamicObject}; }

void set(juce::var &target, const juce::Identifier &key, juce::var value) {
  target.getDynamicObject()->setProperty(key, std::move(value));
}

juce::int64 logCounter(const std::uint64_t value) noexcept {
  return static_cast<juce::int64>(std::min(
      value,
      static_cast<std::uint64_t>(std::numeric_limits<juce::int64>::max())));
}

juce::var scalarEventValue(const int type, const std::byte *bytes) {
  switch (type) {
  case ONDA_PRIMITIVE_BOOL:
    return std::to_integer<std::uint8_t>(*bytes) != 0U;
  case ONDA_PRIMITIVE_F32: {
    float value{};
    std::memcpy(&value, bytes, sizeof(value));
    return static_cast<double>(value);
  }
  case ONDA_PRIMITIVE_F64: {
    double value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
  }
  case ONDA_PRIMITIVE_I32: {
    std::int32_t value{};
    std::memcpy(&value, bytes, sizeof(value));
    return static_cast<int>(value);
  }
  case ONDA_PRIMITIVE_I64: {
    std::int64_t value{};
    std::memcpy(&value, bytes, sizeof(value));
    return juce::String{value};
  }
  default:
    return {};
  }
}

std::size_t eventScalarBytes(const int type) noexcept {
  switch (type) {
  case ONDA_PRIMITIVE_BOOL:
    return 1U;
  case ONDA_PRIMITIVE_F32:
  case ONDA_PRIMITIVE_I32:
    return 4U;
  case ONDA_PRIMITIVE_F64:
  case ONDA_PRIMITIVE_I64:
    return 8U;
  default:
    return 0U;
  }
}

std::string_view eventScalarName(const int type) noexcept {
  switch (type) {
  case ONDA_PRIMITIVE_BOOL:
    return "bool";
  case ONDA_PRIMITIVE_F32:
    return "f32";
  case ONDA_PRIMITIVE_F64:
    return "f64";
  case ONDA_PRIMITIVE_I32:
    return "i32";
  case ONDA_PRIMITIVE_I64:
    return "i64";
  default:
    return {};
  }
}

juce::var defaultEventValue(const EventParameterMapping &parameter) {
  if (parameter.slice)
    return juce::Array<juce::var>{};
  const auto scalarBytes = eventScalarBytes(parameter.elementType);
  const auto zero =
      parameter.elementType == ONDA_PRIMITIVE_BOOL
          ? juce::var{false}
          : (parameter.elementType == ONDA_PRIMITIVE_I64 ? juce::var{"0"}
                                                         : juce::var{0});
  if (!parameter.array) {
    return parameter.defaultBytes.size() == scalarBytes
               ? scalarEventValue(parameter.elementType,
                                  parameter.defaultBytes.data())
               : zero;
  }

  juce::Array<juce::var> values;
  values.ensureStorageAllocated(parameter.arrayLength);
  for (int index = 0; index < parameter.arrayLength; ++index) {
    const auto offset = static_cast<std::size_t>(index) * scalarBytes;
    values.add(offset + scalarBytes <= parameter.defaultBytes.size()
                   ? scalarEventValue(parameter.elementType,
                                      parameter.defaultBytes.data() + offset)
                   : zero);
  }
  return values;
}

} // namespace

std::optional<Resource> runViewResource(const juce::String &request) {
  const auto path = request.upToFirstOccurrenceOf("?", false, false);
  if (path == "/" || path == "/index.html") {
    return utf8Resource(OndaRunResources::run_html,
                        OndaRunResources::run_htmlSize,
                        "text/html; charset=utf-8");
  }
  if (path == "/param-control.js") {
    return utf8Resource(OndaRunResources::paramcontrol_js,
                        OndaRunResources::paramcontrol_jsSize,
                        "text/javascript; charset=utf-8");
  }
  return std::nullopt;
}

juce::var makeRunViewState(Processor &processor, const WorkerStatus &status,
                           const bool canExportProject,
                           const std::string &actionError,
                           const bool resetEventArguments) {
  auto state = object();
  set(state, "running", status.active);
  set(state, "connected", status.active);
  set(state, "path",
      status.path.empty() ? juce::String{} : pathToJuce(status.path));
  set(state, "status",
      status.compiling ? juce::String{"Compiling"}
                       : juce::String(status.message));
  const auto isError = !status.path.empty() && !status.compiling &&
                       status.message != "Active" &&
                       status.message != "Waiting to compile" &&
                       status.message != "Waiting for host specialization";
  set(state, "error",
      !actionError.empty()
          ? juce::String(actionError)
          : (isError ? juce::String(status.message) : juce::String{}));
  set(state, "sourceDirty", false);
  set(state, "supportsSourceSelection", true);
  set(state, "supportsProjectExport", true);
  set(state, "canExportProject", canExportProject);
  set(state, "supportsBufferEmbedding", false);
  set(state, "supportsTransport", false);
  set(state, "supportsDeviceSelection", false);
  set(state, "supportsRunSettings", false);
  set(state, "supportsReset", true);
  set(state, "supportsScope", true);
  auto midi = object();
  const auto noteOn = status.active && status.midi.noteOn;
  const auto noteOff = status.active && status.midi.noteOff;
  set(midi, "available", false);
  set(midi, "noteOn", noteOn);
  set(midi, "noteOff", noteOff);
  set(state, "midi", std::move(midi));
  set(state, "midiKeyboardInteractive", false);
  set(state, "midiInputDevices", juce::Array<juce::var>{});
  set(state, "currentMidiInputDevice", juce::var{});
  set(state, "paramLayout",
      processor.paramControlLayout() == ParamControlLayout::knobs
          ? juce::String{"knobs"}
          : juce::String{"sliders"});
  set(state, "sampleRateHz", processor.hostSampleRate());
  set(state, "blockFrames", processor.hostBlockSize());

  const auto runtimeLog = processor.runtimeLogSnapshot();
  juce::String logText;
  juce::Array<juce::var> logEntries;
  logEntries.ensureStorageAllocated(
      static_cast<int>(runtimeLog.records.size()));
  for (const auto &record : runtimeLog.records) {
    logText += juce::String::fromUTF8(record.text.data(),
                                      static_cast<int>(record.text.size()));
    logText += "\n";
    auto entry = object();
    set(entry, "kind",
        record.kind == RuntimeLogKind::print ? juce::String{"print"}
                                             : juce::String{"delegate"});
    if (!record.sourceFile.empty()) {
      auto source = object();
      set(source, "file",
          juce::String::fromUTF8(record.sourceFile.data(),
                                 static_cast<int>(record.sourceFile.size())));
      set(source, "line", static_cast<juce::int64>(record.line));
      set(entry, "source", std::move(source));
    }
    if (!record.lexicalOwner.empty()) {
      set(entry, "lexicalOwner",
          juce::String::fromUTF8(record.lexicalOwner.data(),
                                 static_cast<int>(record.lexicalOwner.size())));
    }
    logEntries.add(std::move(entry));
  }
  const auto &logCounters = runtimeLog.counters;
  set(state, "logText", std::move(logText));
  set(state, "logEntries", std::move(logEntries));
  set(state, "logRevealed", runtimeLog.revealed);
  set(state, "printOverflowCount", logCounter(logCounters.printOverflow));
  set(state, "printTransportDropCount",
      logCounter(logCounters.printTransportDrops));
  set(state, "delegateOverflowCount", logCounter(logCounters.delegateOverflow));
  set(state, "delegateTransportDropCount",
      logCounter(logCounters.delegateTransportDrops));

  juce::Array<juce::var> buffers;
  buffers.ensureStorageAllocated(static_cast<int>(status.buffers.size()));
  for (const auto &mapping : status.buffers) {
    auto buffer = object();
    set(buffer, "index", mapping.index);
    set(buffer, "name", juce::String(mapping.name));
    set(buffer, "type", juce::String(mapping.type));
    set(buffer, "channelsKind",
        mapping.channelKind == BufferChannelKind::mono
            ? juce::String{"mono"}
            : (mapping.channelKind == BufferChannelKind::fixed
                   ? juce::String{"static"}
                   : juce::String{"dynamic"}));
    set(buffer, "channelsStatic",
        mapping.channelKind == BufferChannelKind::fixed
            ? juce::var{mapping.fixedChannels}
            : juce::var{});
    set(buffer, "loadedPath",
        mapping.loadedPath.empty() ? juce::var{}
                                   : juce::var{pathToJuce(mapping.loadedPath)});
    const auto loaded = mapping.loadedFrames > 0 && mapping.loadedChannels > 0;
    set(buffer, "loadedFrames",
        loaded ? juce::var{mapping.loadedFrames} : juce::var{});
    set(buffer, "loadedChannels",
        loaded ? juce::var{mapping.loadedChannels} : juce::var{});
    set(buffer, "loadedSampleRate",
        loaded ? juce::var{static_cast<double>(mapping.loadedSampleRate)}
               : juce::var{});
    buffers.add(std::move(buffer));
  }
  set(state, "buffers", std::move(buffers));

  juce::Array<juce::var> events;
  events.ensureStorageAllocated(static_cast<int>(status.events.size()));
  for (const auto &mapping : status.events) {
    auto event = object();
    set(event, "index", mapping.index);
    set(event, "name", juce::String(mapping.name));
    juce::Array<juce::var> arguments;
    arguments.ensureStorageAllocated(
        static_cast<int>(mapping.parameters.size()));
    for (std::size_t index = 0; index < mapping.parameters.size(); ++index) {
      const auto &mappingParameter = mapping.parameters[index];
      auto argument = object();
      set(argument, "index", static_cast<int>(index));
      set(argument, "name", juce::String(mappingParameter.name));
      set(argument, "type", juce::String(mappingParameter.type));
      set(argument, "scalar",
          juce::String(eventScalarName(mappingParameter.elementType).data()));
      set(argument, "arrayLength", mappingParameter.arrayLength);
      set(argument, "isSlice", mappingParameter.slice);
      auto defaultValue = defaultEventValue(mappingParameter);
      set(argument, "default", defaultValue);
      if (resetEventArguments)
        set(argument, "value", std::move(defaultValue));
      arguments.add(std::move(argument));
    }
    set(event, "args", std::move(arguments));
    events.add(std::move(event));
  }
  set(state, "events", std::move(events));

  juce::Array<juce::var> parameters;
  parameters.ensureStorageAllocated(static_cast<int>(status.mappings.size()));
  for (std::size_t index = 0; index < status.mappings.size(); ++index) {
    const auto &mapping = status.mappings[index];
    auto parameter = object();
    set(parameter, "name",
        "Slot " + juce::String(static_cast<int>(index + 1U)) + " -> " +
            juce::String(mapping.name));
    set(parameter, "label", juce::String(mapping.name));
    set(parameter, "type", juce::String(mapping.type));
    set(parameter, "normalizedValue",
        static_cast<double>(processor.slotValue(index)));
    set(parameter, "hostValueDomain", "normalized");
    set(parameter, "default", mapping.defaultPlain);
    set(parameter, "rangeMin", mapping.rangeMin);
    set(parameter, "rangeMax", mapping.rangeMax);
    set(parameter, "scale", juce::String(mapping.scale));
    set(parameter, "curve",
        mapping.curve ? juce::var{*mapping.curve} : juce::var{});
    set(parameter, "unit", juce::String(mapping.unit));
    set(parameter, "step",
        mapping.step ? juce::var{*mapping.step} : juce::var{});
    set(parameter, "stepCount",
        mapping.stepCount
            ? juce::var{static_cast<juce::int64>(*mapping.stepCount)}
            : juce::var{});
    parameters.add(std::move(parameter));
  }
  set(state, "params", std::move(parameters));

  auto message = object();
  set(message, "type", "state");
  set(message, "state", std::move(state));
  return message;
}

juce::var makeRunViewScope(Processor &processor) {
  const auto snapshot = processor.scopeSnapshot();
  juce::Array<juce::var> samples;
  samples.ensureStorageAllocated(static_cast<int>(snapshot.samples.size()));
  for (const auto sample : snapshot.samples)
    samples.add(sample);

  auto message = object();
  set(message, "type", "scopeData");
  set(message, "channels", snapshot.channels);
  set(message, "samples", std::move(samples));
  return message;
}

juce::var makeRunViewMidiActivity(const MidiActivitySnapshot &activity) {
  juce::Array<juce::var> activeNotes;
  for (std::size_t note = 0; note < midiNoteCount; ++note) {
    if (activity.active(note))
      activeNotes.add(static_cast<int>(note));
  }

  auto message = object();
  set(message, "type", "midiActivity");
  set(message, "activeNotes", std::move(activeNotes));
  return message;
}

} // namespace onda::plugin
