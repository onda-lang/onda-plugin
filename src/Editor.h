#pragma once

#include "ParameterState.h"

#include "ControlValue.h"
#include "ScopeCapture.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <memory>
#include <string>

namespace onda::plugin {

class Processor;
enum class ParamControlLayout : std::uint8_t;

class Editor final : public juce::AudioProcessorEditor,
                     public juce::FileDragAndDropTarget,
                     private juce::Timer {
public:
  explicit Editor(Processor &owner);
  ~Editor() override;

  void paint(juce::Graphics &graphics) override;
  void resized() override;
  bool isInterestedInFileDrag(const juce::StringArray &files) override;
  void filesDropped(const juce::StringArray &files, int x, int y) override;

private:
  friend struct EditorTestAccess;
  class Browser;

  void timerCallback() override;
  void handleCommand(const juce::var &command);
  void endParameterGesture(std::size_t index);
  void publishState(bool force);
  void publishMidiActivity(bool force = false);
  void publishScope(bool force = false);

  Processor &processor_;
  std::unique_ptr<Browser> browser_;
  juce::Label loadingOverlay_;
  std::unique_ptr<juce::FileChooser> fileChooser_;
  std::uint64_t publishedRevision_{};
  std::uint64_t publishedEngineGeneration_{};
  std::uint64_t publishedLogRevision_{};
  std::uint64_t publishedMidiRevision_{};
  std::uint64_t publishedViewStateRestoreRevision_{};
  int publishedReadyId_{};
  ScopeRevision publishedScopeRevision_{};
  std::array<std::uint64_t, slotCount> publishedSlotStamps_{};
  std::array<std::uint64_t, controlCount> publishedControlStamps_{};
  std::uint64_t publishedBuses_{};
  ParamControlLayout publishedInputLayout_{};
  double publishedInputSmoothing_{};
  bool publishedKnobLayout_{};
  bool publishedFastMath_{};
  double publishedSmoothingSeconds_{};
  int publishedHostBlockSize_{};
  int publishedBlockPreference_{};
  bool hasPublished_{};
  bool hasPublishedMidi_{};
  bool hasPublishedScope_{};
  bool scopeCaptureActive_{};
  bool browserReady_{};
  std::array<bool, slotCount + controlCount> activeGestures_{};
  std::string actionError_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Editor)
};

} // namespace onda::plugin
