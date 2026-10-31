#include "Editor.h"
#include "GpuView.h"

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

namespace onda::plugin {
namespace {

const auto editorBackground = juce::Colour::fromRGB(13, 25, 41);
const auto editorMutedText = juce::Colour::fromRGB(146, 168, 191);

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

bool isBufferFile(const juce::File &file) {
  return std::ranges::any_of(
      supportedAudioFileExtensions(), [&](const auto &extension) {
        return file.hasFileExtension(juce::String::fromUTF8(extension.c_str()));
      });
}

bool isSourcePath(const juce::String &path) {
  return juce::File::isAbsolutePath(path) &&
         juce::File(path).hasFileExtension("onda;on;ondaproject");
}

} // namespace

Editor::Editor(Processor &owner)
    : AudioProcessorEditor(owner), processor_(owner) {
  gpu_ = std::make_unique<GpuView>(
      [this](const juce::var &command) { handleCommand(command); },
      [this] {
        viewReady();
        loadingOverlay_.setVisible(false);
      });
  const auto [width, height] = processor_.editorSize();
  setAccessible(false);
  setOpaque(true);
  addAndMakeVisible(*gpu_);
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
  startTimerHz(60);
}

Editor::~Editor() {
  stopTimer();
  // Release drafts and gestures while their command handlers are still alive.
  gpu_.reset();
  processor_.releaseKeyboardNotes();
  processor_.setScopeCaptureEnabled(false);
  processor_.setMeterCaptureEnabled(false);
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

bool Editor::isInterestedInFileDrag(const juce::StringArray &files) {
  if (std::any_of(files.begin(), files.end(), isSourcePath))
    return true;
  return files.size() == 1 && juce::File::isAbsolutePath(files[0]) &&
         isBufferFile(juce::File(files[0])) &&
         !processor_.workerStatus().bufferChoices().empty();
}

void Editor::publishFileDrag(const char *phase, const juce::StringArray &files,
                             const int x, const int y) {
  if (!viewReady_)
    return;
  juce::var message{new juce::DynamicObject};
  auto &object = *message.getDynamicObject();
  const auto source = std::find_if(files.begin(), files.end(), isSourcePath);
  object.setProperty("type", source != files.end() ? "sourceFileDrag"
                                                   : "bufferFileDrag");
  object.setProperty("phase", phase);
  juce::Array<juce::var> paths;
  if (source != files.end())
    paths.add(*source);
  else
    for (const auto &path : files)
      paths.add(path);
  object.setProperty("paths", std::move(paths));
  juce::var position{new juce::DynamicObject};
  position.getDynamicObject()->setProperty("x", x);
  position.getDynamicObject()->setProperty("y", y);
  object.setProperty("position", std::move(position));
  emitState(std::move(message));
}

void Editor::fileDragEnter(const juce::StringArray &files, int x, int y) {
  publishFileDrag("enter", files, x, y);
}

void Editor::fileDragMove(const juce::StringArray &files, int x, int y) {
  publishFileDrag("over", files, x, y);
}

void Editor::fileDragExit(const juce::StringArray &files) {
  publishFileDrag("leave", files, 0, 0);
}

void Editor::filesDropped(const juce::StringArray &files, int x, int y) {
  fileDragExit(files);
  for (const auto &path : files) {
    if (!isSourcePath(path))
      continue;
    const juce::File file(path);
    if (!file.existsAsFile())
      continue;
    actionError_.clear();
    processor_.loadFile(file, true);
    return;
  }
  if (isInterestedInFileDrag(files))
    publishFileDrag("drop", files, x, y);
}

void Editor::resized() {
  if (gpu_)
    gpu_->setBounds(getLocalBounds());
  loadingOverlay_.setBounds(getLocalBounds());
  processor_.setEditorSize(getWidth(), getHeight());
}

bool Editor::viewVisible() const {
  return gpu_ && gpu_->isVisible();
}

void Editor::emitState(juce::var message) {
  if (gpu_)
    gpu_->message(message);
}

void Editor::viewReady() {
  for (std::size_t index = 0; index < activeGestures_.size(); ++index)
    endParameterGesture(index);
  viewReady_ = true;
  hasPublished_ = false;
  hasPublishedMidi_ = false;
  hasPublishedScope_ = false;
  publishState(true);
  publishMidiActivity(true);
  publishVisualization(true);
}

void Editor::timerCallback() {
  const auto tickGpu = [this] {
    gpu_->tick();
    if (gpu_->error().isNotEmpty()) {
      loadingOverlay_.setText(gpu_->error(), juce::dontSendNotification);
      loadingOverlay_.setVisible(true);
    }
  };
  if (!viewReady_ || !gpu_->isShowing())
    tickGpu();
  const auto showing = viewReady_ && isShowing() && viewVisible();
  const auto captureMeter = showing && gpu_->meterVisible();
  if (meterCaptureActive_ != captureMeter) {
    meterCaptureActive_ = captureMeter;
    processor_.setMeterCaptureEnabled(captureMeter);
    publishedVisualizationTimeMs_ = 0;
  }
  const auto captureScope = showing && gpu_->scopeVisible();
  if (scopeCaptureActive_ != captureScope) {
    scopeCaptureActive_ = captureScope;
    processor_.setScopeCaptureEnabled(captureScope);
    publishedVisualizationTimeMs_ = 0;
  }
  if (!showing) {
    hasPublished_ = false;
    hasPublishedMidi_ = false;
    hasPublishedScope_ = false;
    return;
  }
  publishState(false);
  publishMidiActivity();
  publishVisualization();
  tickGpu();
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
  emitState(makeRunViewMidiActivity(activity));
}

void Editor::publishState(const bool force) {
  if (!viewReady_ || !isShowing() || !viewVisible()) {
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
  const auto currentFastMath = processor_.fastMath();
  if (!force && hasPublished_ && revision == publishedRevision_ &&
      currentBuses == publishedBuses_ &&
      currentInputLayout == publishedInputLayout_ &&
      currentKnobLayout == publishedKnobLayout_ &&
      currentHostBlockSize == publishedHostBlockSize_ &&
      currentBlockPreference == publishedBlockPreference_ &&
      currentFastMath == publishedFastMath_ &&
      viewStateRestoreRevision == publishedViewStateRestoreRevision_) {
    if (!std::is_eq(currentInputSmoothing <=> publishedInputSmoothing_) ||
        !std::is_eq(currentSmoothingSeconds <=> publishedSmoothingSeconds_))
      publishSmoothing();
    if (currentSlots != publishedSlotStamps_ ||
        currentControls != publishedControlStamps_) {
      publishedSlotStamps_ = currentSlots;
      publishedControlStamps_ = currentControls;
      emitState(
          makeRunViewParameterValues(processor_, processor_.workerStatus()));
    }
    if (logRevision != publishedLogRevision_) {
      publishedLogRevision_ = logRevision;
      emitState(makeRunViewLog(processor_));
    }
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
  publishedWorkerError_ = status.hasError ? status.message : std::string{};
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
  publishedFastMath_ = currentFastMath;
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
    // Presentation belongs to its source; an untouched newly loaded source
    // must not inherit a previously saved source's event arguments on reopen.
    if (!viewState.isObject() ||
        (viewState.hasProperty("path") &&
         viewState.getProperty("path", {}).toString() !=
             state.getProperty("path", {}).toString())) {
      viewState = juce::var{new juce::DynamicObject};
      viewState.getDynamicObject()->setProperty("reset", true);
    }
    viewState.getDynamicObject()->setProperty("readyId", ++publishedReadyId_);
    state.getDynamicObject()->setProperty("viewState", std::move(viewState));
  }
  emitState(message);
}

void Editor::publishSmoothing() {
  if (!viewReady_ || !isShowing() || !viewVisible()) {
    hasPublished_ = false;
    return;
  }
  if (!hasPublished_) {
    publishState(true);
    return;
  }
  publishedSmoothingSeconds_ = processor_.paramSmoothingSeconds();
  publishedInputSmoothing_ = processor_.inputControlSmoothingSeconds();
  emitState(makeRunViewSmoothing(
      publishedSmoothingSeconds_, publishedInputSmoothing_,
      actionError_.empty() ? publishedWorkerError_ : actionError_));
}

void Editor::publishVisualization(const bool force) {
  if (!isShowing() || !viewVisible() ||
      (!scopeCaptureActive_ && !meterCaptureActive_))
    return;
  // Meters and waveform share one clock; GPU input remains at 60 Hz.
  constexpr auto intervalMs = 1000.0 / 30.0;
  const auto now = juce::Time::getMillisecondCounterHiRes();
  const auto elapsed = !force && publishedVisualizationTimeMs_ > 0.0
                           ? now - publishedVisualizationTimeMs_
                           : intervalMs;
  if (elapsed < intervalMs)
    return;
  publishedVisualizationTimeMs_ = now - std::fmod(elapsed, intervalMs);
  if (meterCaptureActive_) {
    const auto levels = processor_.meterSnapshot();
    gpu_->meter(pluginOutputChannels, levels.peaks.data(), levels.peaks.size());
  }
  if (!scopeCaptureActive_)
    return;
  const auto revision = processor_.scopeRevision();
  if (!force && hasPublishedScope_ && revision == publishedScopeRevision_)
    return;
  publishedScopeRevision_ = revision;
  hasPublishedScope_ = true;
  const auto snapshot = processor_.scopeSnapshot();
  gpu_->scope(snapshot.channels, snapshot.samples);
}

void Editor::handleCommand(const juce::var &command) {
  const auto *input = command.getDynamicObject();
  if (input == nullptr)
    return;
  const auto type = input->getProperty("type").toString();

  if (type == "viewState") {
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
    publishSmoothing();
  } else if (type == "setInputControlLayout") {
    const auto layout = input->getProperty("layout").toString();
    if (layout == "sliders" || layout == "knobs")
      processor_.setInputControlLayout(layout == "knobs"
                                           ? ParamControlLayout::knobs
                                           : ParamControlLayout::sliders);
  } else if (type == "setFastMath") {
    const auto enabled = input->getProperty("enabled");
    actionError_ =
        enabled.isBool() && processor_.setFastMath(static_cast<bool>(enabled))
            ? std::string{}
            : "Unload the program before changing Fast Math";
    publishState(true);
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
    publishSmoothing();
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
        "Load buffer file", processor_.lastBrowseDirectory(),
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
  } else if (type == "bindBufferFile") {
    const auto name = input->getProperty("name").toString().toStdString();
    const auto path = input->getProperty("filePath");
    if (!hasBuffer(processor_.workerStatus(), name) || !path.isString() ||
        !juce::File::isAbsolutePath(path.toString()))
      return;
    const juce::File file{path.toString()};
    if (isBufferFile(file) && file.existsAsFile()) {
      actionError_.clear();
      processor_.bindBufferFile(name, file);
    }
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
