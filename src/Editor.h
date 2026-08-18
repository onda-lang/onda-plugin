#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <memory>
#include <string>

namespace onda::plugin {

class Processor;

class Editor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
  explicit Editor(Processor &owner);
  ~Editor() override;

  void paint(juce::Graphics &graphics) override;
  void resized() override;

private:
  class Browser;

  void timerCallback() override;
  void handleCommand(const juce::var &command);
  void publishState(bool force);

  Processor &processor_;
  std::unique_ptr<Browser> browser_;
  std::unique_ptr<juce::FileChooser> fileChooser_;
  std::uint64_t publishedRevision_{};
  std::array<float, 32U> publishedSlots_{};
  bool hasPublished_{};
  std::array<bool, 32U> activeGestures_{};
  std::string actionError_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Editor)
};

} // namespace onda::plugin
