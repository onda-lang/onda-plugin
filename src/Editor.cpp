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

const auto editorBackground = juce::Colour::fromRGB(13, 25, 41);
const auto editorMutedText = juce::Colour::fromRGB(146, 168, 191);

constexpr auto juceHostBridgeScript = R"JS(
(() => {
  let parameterDomains = new Map();
  let parameterPath = "";
  const activeParameterGestures = new Set();
  let blockSettingsKey = "";
  let hostBlockSamples = 0;
  let eventDefinitions = [];
  let eventGeneration = "0";

  function parameterControlKey(parameter) {
    const { value, normalizedValue, ...control } = parameter;
    return JSON.stringify(control);
  }

  function createParameterDomain(parameter) {
    if (parameter.rangeMin === parameter.rangeMax) return {
      normalizedToPlain: () => parameter.rangeMin,
      plainToNormalized: () => parameter.normalizedValue,
    };
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
    const structured = typeof argument?.type === "string"
      && !/^(?:f32|f64|i32|i64|bool)(?:\[[0-9]*\])?$/.test(argument.type);
    if (structured) {
      const value = argument.default ?? {};
      return typeof structuredClone === "function"
        ? structuredClone(value)
        : JSON.parse(JSON.stringify(value));
    }
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
    if (argument?.type === "i64") {
      return typeof argument.default === "string" ? argument.default : "0";
    }
    if ((argument?.type === "f32" || argument?.type === "f64")
        && ["NaN", "Infinity", "-Infinity"].includes(argument.default)) {
      return argument.default;
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
    window._onHostMessage({
      type: "state", state: { events, resetEventArguments: true },
    });
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
      if (message?.type === "triggerEvent") {
        message.engineGeneration = eventGeneration;
      }
      if (message?.type === "setRunSettings" && message.blockSamples === hostBlockSamples) {
        message.blockSamples = 0;
      }
      if (message?.type === "setParam" || message?.type === "setInputControl") {
        const binding = parameterDomains.get(message.name);
        if (!binding) return;
        message.value = binding.domain.plainToNormalized(message.value);
        if (message.commit) activeParameterGestures.delete(message.name);
        else activeParameterGestures.add(message.name);
      }
      window.__JUCE__.backend.emitEvent("ondaCommand", message);
    },
  };

  document.addEventListener("DOMContentLoaded", () => {
    const sampleRate = document.getElementById("run-sample-rate");
    if (sampleRate) {
      sampleRate.disabled = true;
      sampleRate.closest(".param-setting").hidden = true;
    }
    const runSettings = document.getElementById("file-picker-settings");
    if (runSettings) runSettings.style.gridTemplateColumns = "minmax(0, 1fr)";
    const blockLabel = document.querySelector('label[for="run-block-size"]');
    if (blockLabel) blockLabel.textContent = "Onda Block Size";
    const input = document.getElementById("param-smoothing-ms");
    if (input) {
      input.setAttribute("aria-label", "Editor parameter smoothing duration in milliseconds");
      input.setAttribute("title", "Smooths editor changes. DAW automation updates at block boundaries. 0 disables smoothing.");
    }
    updateBlockSettings({ hostBlockSamples: 0, canSetBlockSize: false });
  });

  function updateBlockSettings(state) {
    if (!Number.isInteger(state.hostBlockSamples)) return;
    const select = document.getElementById("run-block-size");
    if (!select) return;
    const maximum = state.hostBlockSamples;
    hostBlockSamples = maximum;
    const preference = state.preferredBlockSamples ?? 0;
    const key = `${maximum}:${preference}:${state.canSetBlockSize}`;
    if (key !== blockSettingsKey) {
      const options = [];
      const add = (value, text) => {
        const option = document.createElement("option");
        option.value = String(value);
        option.textContent = text;
        options.push(option);
        return option;
      };
      add(maximum, maximum > 0 ? `Host (${maximum} samples)` : "Host (waiting)");
      const sizes = new Set([maximum]);
      for (const divisor of [2, 4, 8]) {
        const size = Math.floor(maximum / divisor);
        if (size > 0 && !sizes.has(size)) {
          sizes.add(size);
          add(size, `${size} samples`);
        }
      }
      // Host capacity changes can leave the active fixed size outside these
      // choices. Display it without offering it as another menu choice.
      if (preference > 0 && !sizes.has(preference)) {
        const option = add(preference, preference > maximum
          ? `${preference} (using ${maximum})` : `${preference} samples`);
        option.hidden = option.disabled = true;
      }
      select.replaceChildren(...options);
      blockSettingsKey = key;
    }
    select.value = String(preference || maximum);
    select.disabled = state.canSetBlockSize !== true || maximum <= 0;
    select.title = "Choose before loading a program. Smaller Onda blocks update automation more often and use more CPU.";
  }

  window.__JUCE__.backend.addEventListener("ondaState", (message) => {
    if (message?.type === "state" && Array.isArray(message.state?.events)) {
      eventDefinitions = message.state.events;
      eventGeneration = message.state.engineGeneration;
    }
    if (message?.type === "state" && (Array.isArray(message.state?.params) || Array.isArray(message.state?.inputControls))) {
      const nextPath = message.state.path ?? parameterPath;
      const nextDomains = new Map([...parameterDomains].filter(([, binding]) =>
        nextPath === parameterPath && !Array.isArray(message.state[binding.bank])));
      for (const [bank, endCommand] of [["params", "endParamGesture"], ["inputControls", "endInputControlGesture"]]) {
        for (const parameter of message.state[bank] || []) {
          if (parameter.hostValueDomain !== "normalized") {
            continue;
          }
          try {
            const domain = createParameterDomain(parameter);
            nextDomains.set(parameter.name, {
              domain, controlKey: parameterControlKey(parameter), endCommand, bank,
            });
            if (typeof parameter.value !== "number"
                && !(parameter.type === "i64" && typeof parameter.value === "string"))
              parameter.value = domain.normalizedToPlain(parameter.normalizedValue);
          } catch {
            // The shared view will surface malformed parameter metadata.
          }
        }
      }
      // The shared view discards obsolete drags without committing their value.
      // Release the DAW gesture separately before the controls are replaced.
      for (const name of activeParameterGestures) {
        if (nextPath !== parameterPath || !nextDomains.has(name)
            || nextDomains.get(name).controlKey !== parameterDomains.get(name)?.controlKey) {
          activeParameterGestures.delete(name);
          window.__JUCE__.backend.emitEvent("ondaCommand", {
            type: parameterDomains.get(name)?.endCommand || "endParamGesture", name,
          });
        }
      }
      parameterDomains = nextDomains;
      parameterPath = nextPath;
    }
    if (typeof window._onHostMessage === "function") {
      window._onHostMessage(message);
    }
    if (message?.type === "state") updateBlockSettings(message.state);
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

std::optional<std::size_t> parameterIndex(std::string_view name,
                                          const std::string_view prefix,
                                          const std::size_t count) {
  if (!name.starts_with(prefix))
    return std::nullopt;
  const auto numberEnd = name.find_first_not_of("0123456789", prefix.size());
  const auto numberText =
      name.substr(prefix.size(), numberEnd == std::string_view::npos
                                     ? name.size() - prefix.size()
                                     : numberEnd - prefix.size());
  std::size_t value{};
  const auto parsed = std::from_chars(
      numberText.data(), numberText.data() + numberText.size(), value);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != numberText.data() + numberText.size() || value == 0U ||
      value > count) {
    return std::nullopt;
  }
  return value - 1U;
}

std::optional<std::uint64_t> parseGeneration(const juce::var &value) {
  if (!value.isString())
    return std::nullopt;
  const auto text = value.toString().toStdString();
  std::uint64_t generation{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), generation);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    return std::nullopt;
  return generation;
}

bool hasBuffer(const WorkerStatus &status, const std::string_view name) {
  return std::any_of(
      status.bufferChoices().begin(), status.bufferChoices().end(),
      [name](const BufferMapping &buffer) { return buffer.name == name; });
}

juce::WebBrowserComponent::Options browserOptions() {
  juce::WebBrowserComponent::Options options;
  options = options.withUserScript(juceHostBridgeScript);
#if JUCE_WINDOWS
  const auto dataFolder =
      juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
          .getChildFile("Onda/VST3/WebView2");
  options =
      options.withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
          .withWinWebView2Options(
              juce::WebBrowserComponent::Options::WinWebView2{}
                  .withUserDataFolder(dataFolder));
#endif
  return options;
}

} // namespace

class Editor::Browser final : public juce::WebBrowserComponent {
public:
  explicit Browser(Editor &owner)
      : WebBrowserComponent(
            browserOptions()
                .withNativeIntegrationEnabled()
                .withKeepPageLoadedWhenBrowserIsHidden()
                .withEventListener("ondaCommand",
                                   [&owner](juce::var command) {
                                     owner.handleCommand(command);
                                   })
                .withResourceProvider([](const juce::String &path) {
                  return runViewResource(path);
                })),
        owner_(owner) {}

  [[nodiscard]] juce::String initializationError() const {
#if JUCE_WINDOWS
    const auto options = browserOptions();
    if (const auto result = options.getWinWebView2BackendOptions()
                                .getUserDataFolder()
                                .createDirectory();
        result.failed())
      return "Could not create the Onda browser data folder: " +
             result.getErrorMessage();
    if (!areOptionsSupported(options))
      return "The Onda editor requires Microsoft Edge WebView2 Runtime";
#endif
    return {};
  }

  bool pageAboutToLoad(const juce::String &url) override {
    return url.startsWith(getResourceProviderRoot());
  }

  void newWindowAttemptingToLoad(const juce::String &) override {}

  bool pageLoadHadNetworkError(const juce::String &error) override {
    owner_.loadingOverlay_.setText("Could not load the Onda editor: " + error,
                                   juce::dontSendNotification);
    owner_.loadingOverlay_.setVisible(true);
    return false;
  }

private:
  Editor &owner_;
};

Editor::Editor(Processor &owner)
    : AudioProcessorEditor(owner), processor_(owner),
      browser_(std::make_unique<Browser>(*this)) {
  const auto [width, height] = processor_.editorSize();
  setOpaque(true);
  addAndMakeVisible(*browser_);
  loadingOverlay_.setComponentID("onda-loading-overlay");
  loadingOverlay_.setText("Loading Onda...", juce::dontSendNotification);
  loadingOverlay_.setJustificationType(juce::Justification::centred);
  loadingOverlay_.setColour(juce::Label::backgroundColourId, editorBackground);
  loadingOverlay_.setColour(juce::Label::textColourId, editorMutedText);
  loadingOverlay_.setOpaque(true);
  addAndMakeVisible(loadingOverlay_);
  setResizable(true, true);
  setSize(width, height);
  setResizeLimits(360, 480, 1600, 1400);
  if (const auto error = browser_->initializationError(); error.isNotEmpty())
    loadingOverlay_.setText(error, juce::dontSendNotification);
  else
    browser_->goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
  startTimerHz(20);
}

Editor::~Editor() {
  stopTimer();
  processor_.releaseKeyboardNotes();
  processor_.setScopeCaptureEnabled(false);
  for (std::size_t index = 0; index < activeGestures_.size(); ++index)
    endParameterGesture(index);
  processor_.setEditorSize(getWidth(), getHeight());
}

void Editor::endParameterGesture(const std::size_t index) {
  if (!activeGestures_[index])
    return;
  activeGestures_[index] = false;
  if (index < slotCount)
    processor_.endSlotGesture(index);
  else
    processor_.endInputControlGesture(index - slotCount);
}

void Editor::paint(juce::Graphics &graphics) {
  graphics.fillAll(editorBackground);
}

void Editor::resized() {
  browser_->setBounds(getLocalBounds());
  loadingOverlay_.setBounds(getLocalBounds());
  processor_.setEditorSize(getWidth(), getHeight());
}

void Editor::timerCallback() {
  const auto showing = browserReady_ && isShowing() && browser_->isVisible();
  if (scopeCaptureActive_ != showing) {
    scopeCaptureActive_ = showing;
    processor_.setScopeCaptureEnabled(showing);
  }
  if (!showing) {
    hasPublished_ = false;
    hasPublishedMidi_ = false;
    hasPublishedScope_ = false;
    return;
  }
  publishState(false);
  publishMidiActivity();
  publishScope();
}

void Editor::publishMidiActivity(const bool force) {
  if (!isShowing()) {
    hasPublishedMidi_ = false;
    return;
  }
  const auto activity = processor_.midiActivitySnapshot();
  if (!force && hasPublishedMidi_ &&
      activity.revision == publishedMidiRevision_) {
    return;
  }
  publishedMidiRevision_ = activity.revision;
  hasPublishedMidi_ = true;
  browser_->emitEventIfBrowserIsVisible("ondaState",
                                        makeRunViewMidiActivity(activity));
}

void Editor::publishState(const bool force) {
  if (!browserReady_ || !isShowing() || !browser_->isVisible()) {
    hasPublished_ = false;
    return;
  }
  const auto revision = processor_.workerStatusRevision();
  const auto logRevision = processor_.runtimeLogRevision();
  const auto viewStateRestoreRevision = processor_.viewStateRestoreRevision();
  std::array<std::uint64_t, slotCount> currentSlots{};
  for (std::size_t index = 0; index < currentSlots.size(); ++index)
    currentSlots[index] = processor_.slotStamp(index);
  std::array<std::uint64_t, controlCount> currentControls{};
  std::uint64_t currentBuses{};
  for (std::size_t index = 0; index < controlCount; ++index) {
    currentControls[index] = processor_.inputControlStamp(index);
    if (processor_.getChannelCountOfBus(true, pluginMainInputBuses +
                                                  static_cast<int>(index)) != 0)
      currentBuses |= std::uint64_t{1} << index;
  }
  const auto currentInputLayout = processor_.inputControlLayout();
  const auto currentInputSmoothing = processor_.inputControlSmoothingSeconds();
  const auto currentKnobLayout =
      processor_.paramControlLayout() == ParamControlLayout::knobs;
  const auto currentSmoothingSeconds = processor_.paramSmoothingSeconds();
  const auto currentHostBlockSize = processor_.hostBlockSize();
  const auto currentBlockPreference = processor_.preferredBlockSize();
  if (!force && hasPublished_ && revision == publishedRevision_ &&
      logRevision == publishedLogRevision_ &&
      currentSlots == publishedSlotStamps_ &&
      currentControls == publishedControlStamps_ &&
      currentBuses == publishedBuses_ &&
      currentInputLayout == publishedInputLayout_ &&
      std::is_eq(currentInputSmoothing <=> publishedInputSmoothing_) &&
      currentKnobLayout == publishedKnobLayout_ &&
      std::is_eq(currentSmoothingSeconds <=> publishedSmoothingSeconds_) &&
      currentHostBlockSize == publishedHostBlockSize_ &&
      currentBlockPreference == publishedBlockPreference_ &&
      viewStateRestoreRevision == publishedViewStateRestoreRevision_) {
    return;
  }
  const auto status = processor_.workerStatus();
  const auto resetEventArguments =
      status.engineGeneration != publishedEngineGeneration_ ||
      viewStateRestoreRevision != publishedViewStateRestoreRevision_;
  const auto includeRuntimeLog =
      force || !hasPublished_ || logRevision != publishedLogRevision_;
  const auto initializeViewState =
      !hasPublished_ ||
      viewStateRestoreRevision != publishedViewStateRestoreRevision_;
  publishedEngineGeneration_ = status.engineGeneration;
  publishedRevision_ = status.revision;
  publishedLogRevision_ = logRevision;
  publishedSlotStamps_ = currentSlots;
  publishedControlStamps_ = currentControls;
  publishedBuses_ = currentBuses;
  publishedInputLayout_ = currentInputLayout;
  publishedInputSmoothing_ = currentInputSmoothing;
  publishedKnobLayout_ = currentKnobLayout;
  publishedSmoothingSeconds_ = currentSmoothingSeconds;
  publishedHostBlockSize_ = currentHostBlockSize;
  publishedBlockPreference_ = currentBlockPreference;
  publishedViewStateRestoreRevision_ = viewStateRestoreRevision;
  hasPublished_ = true;
  auto message =
      makeRunViewState(processor_, status, processor_.canExportProject(),
                       actionError_, resetEventArguments, includeRuntimeLog);
  if (initializeViewState) {
    auto state = message.getDynamicObject()->getProperty("state");
    juce::var viewState;
    if (const auto saved = processor_.viewState(); saved.isNotEmpty()) {
      viewState = juce::JSON::parse(saved);
    } else {
      viewState = juce::var{new juce::DynamicObject};
      viewState.getDynamicObject()->setProperty("reset", true);
    }
    viewState.getDynamicObject()->setProperty("readyId", ++publishedReadyId_);
    state.getDynamicObject()->setProperty("viewState", std::move(viewState));
  }
  browser_->emitEventIfBrowserIsVisible("ondaState", message);
}

void Editor::publishScope(const bool force) {
  if (!isShowing() || !browser_->isVisible()) {
    hasPublishedScope_ = false;
    return;
  }
  const auto revision = processor_.scopeRevision();
  if (!force && hasPublishedScope_ && revision == publishedScopeRevision_)
    return;
  publishedScopeRevision_ = revision;
  hasPublishedScope_ = true;
  browser_->emitEventIfBrowserIsVisible("ondaState",
                                        makeRunViewScope(processor_));
}

void Editor::handleCommand(const juce::var &command) {
  const auto *input = command.getDynamicObject();
  if (input == nullptr)
    return;
  const auto type = input->getProperty("type").toString();

  if (type == "webviewReady") {
    for (std::size_t index = 0; index < activeGestures_.size(); ++index)
      endParameterGesture(index);
    browserReady_ = true;
    hasPublished_ = false;
    hasPublishedMidi_ = false;
    hasPublishedScope_ = false;
    publishState(true);
    publishMidiActivity(true);
    publishScope(true);
  } else if (type == "runViewReady") {
    const auto readyId = input->getProperty("readyId");
    if (browserReady_ && (readyId.isInt() || readyId.isInt64()) &&
        static_cast<juce::int64>(readyId) == publishedReadyId_)
      loadingOverlay_.setVisible(false);
  } else if (type == "viewState") {
    processor_.setViewState(input->getProperty("state"));
  } else if (type == "midiNote") {
    const auto key = input->getProperty("key");
    const auto velocity = input->getProperty("velocity");
    const auto pressed = input->getProperty("pressed");
    if (key.isInt() && (velocity.isDouble() || velocity.isInt()) &&
        pressed.isBool())
      processor_.triggerMidiNote(static_cast<int>(key),
                                 static_cast<float>(velocity),
                                 static_cast<bool>(pressed));
  } else if (type == "clearLog") {
    processor_.clearRuntimeLog();
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
    processor_.resetParametersToDefaults(ParameterReset::parameters);
  } else if (type == "resetInputControls") {
    processor_.resetParametersToDefaults(ParameterReset::controls);
  } else if (type == "setInputControlSmoothing") {
    const auto milliseconds = number(input->getProperty("milliseconds"));
    actionError_ = milliseconds && processor_.setInputControlSmoothingSeconds(
                                       *milliseconds / 1000.0)
                       ? std::string{}
                       : "Input control smoothing duration must be finite and "
                         "non-negative";
    publishState(true);
  } else if (type == "setInputControlLayout") {
    const auto layout = input->getProperty("layout").toString();
    if (layout == "sliders" || layout == "knobs")
      processor_.setInputControlLayout(layout == "knobs"
                                           ? ParamControlLayout::knobs
                                           : ParamControlLayout::sliders);
  } else if (type == "setRunSettings") {
    const auto samples = input->getProperty("blockSamples");
    actionError_ = samples.isInt() && processor_.setPreferredBlockSize(
                                          static_cast<int>(samples))
                       ? std::string{}
                       : "Unload the program before choosing an Onda block "
                         "size within the host limit";
    publishState(true);
  } else if (type == "setParamSmoothing") {
    const auto milliseconds = number(input->getProperty("milliseconds"));
    actionError_ = milliseconds && processor_.setParamSmoothingSeconds(
                                       *milliseconds / 1000.0)
                       ? std::string{}
                       : "Smoothing duration must be finite, non-negative, and "
                         "fit the host sample counter";
    publishState(true);
  } else if (type == "reset") {
    processor_.requestUserReset();
  } else if (type == "triggerEvent") {
    const auto generation =
        parseGeneration(input->getProperty("engineGeneration"));
    auto error = generation
                     ? processor_.triggerEvent(
                           *generation,
                           input->getProperty("name").toString().toStdString(),
                           input->getProperty("values"))
                     : "Invalid Onda event generation";
    if (error != actionError_) {
      actionError_ = std::move(error);
      publishState(true);
    }
  } else if (type == "setParamLayout") {
    const auto layout = input->getProperty("layout").toString();
    if (layout == "sliders")
      processor_.setParamControlLayout(ParamControlLayout::sliders);
    else if (layout == "knobs")
      processor_.setParamControlLayout(ParamControlLayout::knobs);
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
  } else if (type == "endParamGesture" || type == "endInputControlGesture" ||
             type == "setParam" || type == "setInputControl") {
    const auto controls =
        type == "endInputControlGesture" || type == "setInputControl";
    const auto lane = parameterIndex(
        input->getProperty("name").toString().toStdString(),
        controls ? "Input " : "Slot ", controls ? controlCount : slotCount);
    if (!lane)
      return;
    const auto index = *lane + (controls ? slotCount : 0U);
    if (type.startsWith("end")) {
      endParameterGesture(index);
      return;
    }
    if (controls &&
        processor_.getChannelCountOfBus(
            true, pluginMainInputBuses + static_cast<int>(*lane)) != 0) {
      endParameterGesture(index);
      return;
    }
    const auto value = number(input->getProperty("value"));
    if (!value)
      return;
    if (!activeGestures_[index]) {
      if (controls)
        processor_.beginInputControlGesture(*lane);
      else
        processor_.beginSlotGesture(*lane);
      activeGestures_[index] = true;
    }
    if (controls)
      processor_.setInputControlValue(*lane, static_cast<float>(*value));
    else
      processor_.setSlotValue(*lane, static_cast<float>(*value));
    if (static_cast<bool>(input->getProperty("commit")))
      endParameterGesture(index);
  }
}

} // namespace onda::plugin
