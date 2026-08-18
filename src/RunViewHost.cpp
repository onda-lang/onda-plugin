#include "RunViewHost.h"

#include "JucePath.h"
#include "Processor.h"

#include <OndaRunResources.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
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
                           const std::string &actionError) {
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
  set(state, "supportsScope", false);
  set(state, "sampleRateHz", processor.hostSampleRate());
  set(state, "blockFrames", processor.hostBlockSize());

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
  set(state, "events", juce::Array<juce::var>{});

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

} // namespace onda::plugin
