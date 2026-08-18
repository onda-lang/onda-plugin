#include "Editor.h"

#include "AudioFile.h"
#include "JucePath.h"
#include "Processor.h"
#include "RunViewHost.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <system_error>

#if JUCE_LINUX && !JUCE_USE_EXTERNAL_TEMPORARY_SUBPROCESS
#error "Linux plugin editors require JUCE's external WebKit subprocess helper"
#endif

namespace onda::plugin {
namespace {

constexpr auto juceHostBridgeScript = R"JS(
(() => {
  const parameterDomains = new Map();
  let eventDefinitions = [];

  function createParameterDomain(parameter) {
    return globalThis.__ONDA_PARAM_CONTROL_V2__.createParamDomain({
      name: parameter.name,
      scalar: parameter.type,
      minimum: parameter.rangeMin,
      maximum: parameter.rangeMax,
      scale: parameter.scale,
      curve: parameter.curve,
      unit: parameter.unit,
      step: parameter.step,
      stepCount: parameter.stepCount,
    });
  }

  function defaultEventArgumentValue(argument) {
    const isArray = Boolean(
      argument?.isSlice
      || (typeof argument?.arrayLength === "number" && argument.arrayLength !== 1)
      || (typeof argument?.type === "string" && /\[[0-9]*\]$/.test(argument.type))
    );
    if (isArray) {
      return Array.isArray(argument.default) ? argument.default.slice() : [];
    }
    if (argument?.type === "bool") {
      return Boolean(argument.default);
    }
    const value = Number(argument?.default);
    return Number.isFinite(value) ? value : 0;
  }

  function resetEventArguments() {
    if (typeof window._onHostMessage !== "function") {
      return;
    }
    const events = eventDefinitions.map((event) => ({
      ...event,
      args: Array.isArray(event.args)
        ? event.args.map((argument) => ({
            ...argument,
            value: defaultEventArgumentValue(argument),
          }))
        : [],
    }));
    window._onHostMessage({ type: "state", state: { events } });
  }

  window.__hostBridge = { mode: "wry" };
  window.ipc = {
    postMessage(payload) {
      let message;
      try {
        message = typeof payload === "string" ? JSON.parse(payload) : payload;
      } catch {
        return;
      }

      if (message?.type === "resetEventArguments") {
        resetEventArguments();
        return;
      }
      if (message?.type === "setParam") {
        const domain = parameterDomains.get(message.name);
        if (domain) {
          message.value = domain.plainToNormalized(message.value);
        }
      }
      window.__JUCE__.backend.emitEvent("ondaCommand", message);
    },
  };

  window.__JUCE__.backend.addEventListener("ondaState", (message) => {
    if (message?.type === "state" && Array.isArray(message.state?.events)) {
      eventDefinitions = message.state.events;
    }
    if (message?.type === "state" && Array.isArray(message.state?.params)) {
      parameterDomains.clear();
      for (const parameter of message.state.params) {
        if (parameter.hostValueDomain !== "normalized") {
          continue;
        }
        try {
          const domain = createParameterDomain(parameter);
          parameterDomains.set(parameter.name, domain);
          parameter.value = domain.normalizedToPlain(parameter.normalizedValue);
        } catch {
          // The shared view will surface malformed parameter metadata.
        }
      }
    }
    if (typeof window._onHostMessage === "function") {
      window._onHostMessage(message);
    }
  });
})();
)JS";

std::optional<double> number(const juce::var &value) {
  if (!value.isInt() && !value.isInt64() && !value.isDouble() &&
      !value.isBool()) {
    return std::nullopt;
  }
  const auto converted = static_cast<double>(value);
  return std::isfinite(converted) ? std::optional<double>{converted}
                                  : std::nullopt;
}

std::optional<std::size_t> slotIndex(std::string_view name) {
  if (!name.starts_with("Slot "))
    return std::nullopt;
  const auto numberEnd = name.find_first_not_of("0123456789", 5U);
  const auto numberText =
      name.substr(5U, numberEnd == std::string_view::npos ? name.size() - 5U
                                                          : numberEnd - 5U);
  std::size_t value{};
  const auto parsed = std::from_chars(
      numberText.data(), numberText.data() + numberText.size(), value);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != numberText.data() + numberText.size() || value == 0U ||
      value > slotCount) {
    return std::nullopt;
  }
  return value - 1U;
}

bool hasBuffer(const WorkerStatus &status, const std::string_view name) {
  return std::any_of(
      status.buffers.begin(), status.buffers.end(),
      [name](const BufferMapping &buffer) { return buffer.name == name; });
}

} // namespace

class Editor::Browser final : public juce::WebBrowserComponent {
public:
  explicit Browser(Editor &owner)
      : WebBrowserComponent(
            Options{}
                .withNativeIntegrationEnabled()
                .withKeepPageLoadedWhenBrowserIsHidden()
                .withUserScript(juceHostBridgeScript)
                .withEventListener("ondaCommand",
                                   [&owner](juce::var command) {
                                     owner.handleCommand(command);
                                   })
                .withResourceProvider([](const juce::String &path) {
                  return runViewResource(path);
                })) {}

  bool pageAboutToLoad(const juce::String &url) override {
    return url.startsWith(getResourceProviderRoot());
  }

  void newWindowAttemptingToLoad(const juce::String &) override {}
};

Editor::Editor(Processor &owner)
    : AudioProcessorEditor(owner), processor_(owner),
      browser_(std::make_unique<Browser>(*this)) {
  setOpaque(true);
  addAndMakeVisible(*browser_);
  setResizable(true, true);
  setResizeLimits(360, 480, 1600, 1400);
  const auto [width, height] = processor_.editorSize();
  setSize(width, height);
  browser_->goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
  startTimerHz(20);
}

Editor::~Editor() {
  stopTimer();
  for (std::size_t index = 0; index < activeGestures_.size(); ++index) {
    if (activeGestures_[index])
      processor_.endSlotGesture(index);
  }
  processor_.setEditorSize(getWidth(), getHeight());
}

void Editor::paint(juce::Graphics &graphics) {
  graphics.fillAll(juce::Colour::fromRGB(12, 30, 51));
}

void Editor::resized() {
  browser_->setBounds(getLocalBounds());
  processor_.setEditorSize(getWidth(), getHeight());
}

void Editor::timerCallback() { publishState(false); }

void Editor::publishState(const bool force) {
  const auto revision = processor_.workerStatusRevision();
  std::array<float, slotCount> currentSlots{};
  for (std::size_t index = 0; index < currentSlots.size(); ++index)
    currentSlots[index] = processor_.slotValue(index);
  if (!force && hasPublished_ && revision == publishedRevision_ &&
      currentSlots == publishedSlots_) {
    return;
  }
  const auto status = processor_.workerStatus();
  publishedRevision_ = status.revision;
  publishedSlots_ = currentSlots;
  hasPublished_ = true;
  browser_->emitEventIfBrowserIsVisible(
      "ondaState",
      makeRunViewState(processor_, status, processor_.canExportProject(),
                       actionError_));
}

void Editor::handleCommand(const juce::var &command) {
  const auto *input = command.getDynamicObject();
  if (input == nullptr)
    return;
  const auto type = input->getProperty("type").toString();

  if (type == "webviewReady") {
    publishState(true);
  } else if (type == "chooseOndaFile") {
    actionError_.clear();
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Load an Onda source or project", processor_.lastBrowseDirectory(),
        "*.onda;*.on;*.ondaproject");
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode |
                                  juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer<Editor>(
                                   this)](const juce::FileChooser &chooser) {
                                if (safe == nullptr)
                                  return;
                                const auto file = chooser.getResult();
                                if (file.existsAsFile())
                                  safe->processor_.loadFile(file, true);
                                safe->fileChooser_.reset();
                              });
  } else if (type == "saveProjectAs") {
    actionError_.clear();
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Save the Onda project in a new or empty folder",
        processor_.lastBrowseDirectory());
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode |
            juce::FileBrowserComponent::canSelectDirectories |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<Editor>(this)](
            const juce::FileChooser &chooser) {
          if (safe == nullptr)
            return;
          const auto directory = chooser.getResult();
          safe->fileChooser_.reset();
          if (directory.getFullPathName().isEmpty())
            return;
          if (!safe->processor_.saveProjectAsAsync(
                  directory, [safe](std::string error) {
                    if (safe == nullptr)
                      return;
                    safe->actionError_ = std::move(error);
                    safe->publishState(true);
                  })) {
            safe->actionError_ = "A project export is already in progress";
            safe->publishState(true);
          }
        });
  } else if (type == "unload") {
    actionError_.clear();
    processor_.unload();
  } else if (type == "resetParams") {
    processor_.resetParametersToDefaults();
  } else if (type == "reset") {
    processor_.requestUserReset();
  } else if (type == "chooseBufferFile") {
    const auto name = input->getProperty("name").toString().toStdString();
    if (!hasBuffer(processor_.workerStatus(), name))
      return;
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Bind an audio file", processor_.lastBrowseDirectory(),
        juce::String::fromUTF8(supportedAudioFileWildcard().c_str()));
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<Editor>(this),
         name](const juce::FileChooser &chooser) {
          if (safe == nullptr)
            return;
          const auto file = chooser.getResult();
          if (file.existsAsFile())
            safe->processor_.bindBufferFile(name, file);
          safe->fileChooser_.reset();
        });
  } else if (type == "clearBuffer") {
    const auto name = input->getProperty("name").toString().toStdString();
    if (hasBuffer(processor_.workerStatus(), name))
      processor_.clearBuffer(name);
  } else if (type == "setParam") {
    const auto index =
        slotIndex(input->getProperty("name").toString().toStdString());
    const auto value = number(input->getProperty("value"));
    if (index && value) {
      const auto commit = static_cast<bool>(input->getProperty("commit"));
      if (!activeGestures_[*index]) {
        processor_.beginSlotGesture(*index);
        activeGestures_[*index] = true;
      }
      processor_.setSlotValue(*index, static_cast<float>(*value));
      if (commit && activeGestures_[*index]) {
        processor_.endSlotGesture(*index);
        activeGestures_[*index] = false;
      }
    }
  }
}

} // namespace onda::plugin
