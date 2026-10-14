#pragma once

#include "ControlInputTestSupport.h"
#include "TestAutomationQueue.h"

namespace test {

inline juce::String callbackProcessingSource() {
  using namespace onda::plugin;
  juce::String source;
  if constexpr (pluginControlInputs > 0)
    source = "ins {\n" + mainAudioInputDeclaration() +
             "modulation = 0.0 {0.0, 1.0}\n}\n";
  source += "outs { out1";
  if constexpr (pluginOutputChannels > 1)
    source += ", out2";
  source += "}\nparams { level = 0.0 {0.0, 1.0}; editor = 0.0 {0.0, 1.0} }\n"
            "init { blocks = 0; finished = 0; captured = 0.0 }\n"
            "event note_on(id: i32, channel: i32, key: i32, velocity: f32) { "
            "captured = level }\n"
            "event note_off(id: i32, channel: i32, key: i32, velocity: f32) { "
            "captured = 0.0 }\n"
            "block { blocks += 1; cached = level; cached_editor = editor\n"
            "sample { out1 = cached + captured";
  if constexpr (pluginControlInputs > 0)
    source += " + modulation";
  if constexpr (pluginOutputChannels > 1)
    source += "\nout2 = f32(blocks) + f32(finished) / 100.0 + cached_editor";
  return source + "\n}\nfinished += 1\n}\n";
}

// The reference schedules chunks afresh for every callback. Its persistent
// counters check both block hooks, including short tails and MIDI splits;
// their persistence and the note state also catch DSP reinitialization.
template <typename Render>
bool checkCallbackProcessing(juce::AudioProcessor &processor, const int block,
                             const bool routed, Render render) {
  using namespace onda::plugin;
  int blocks{};
  float captured{};
  constexpr std::array sizes{6, 2, 8, 0, 1, 5, 7, 3, 8, 2, 6};
  for (std::size_t callback = 0; callback < sizes.size(); ++callback) {
    const auto frames = sizes[callback];
    const auto start = callback % 4U < 2U ? 1.0 : 0.0;
    const auto end = 1.0 - start;
    const auto valueAt = [&](const int offset) {
      return frames <= 1
                 ? end
                 : std::lerp(start, end,
                             static_cast<double>(offset) / (frames - 1));
    };
    const std::array level{AutomationPoint{0, frames <= 1 ? end : start},
                           AutomationPoint{std::max(0, frames - 1), end}};
    const std::array control{AutomationPoint{0, frames <= 1 ? start : end},
                             AutomationPoint{std::max(0, frames - 1), start}};
    const auto eventOffset = frames / 2;
    const auto release = callback == 5U;
    juce::MidiBuffer midi;
    midi.addEvent(release ? juce::MidiMessage::noteOff(1, 60)
                          : juce::MidiMessage::noteOn(1, 60, 1.0F),
                  eventOffset);
    juce::AudioBuffer<float> audio(
        std::max(processor.getTotalNumInputChannels(), pluginOutputChannels),
        frames);
    audio.clear();
    if (routed)
      for (int frame = 0; frame < frames; ++frame)
        audio.setSample(pluginInputChannels, frame,
                        0.125F + static_cast<float>(frame) / 16.0F);
    if (!render(audio, midi, level, control))
      return false;
    if (frames == 0)
      captured = static_cast<float>(end);
    for (int frame = 0; frame < frames; ++frame) {
      const auto chunkStart = frame / block * block;
      if (frame % block == 0)
        ++blocks;
      if (frame == eventOffset)
        captured = release ? 0.0F : static_cast<float>(valueAt(chunkStart));
      const auto modulation = pluginControlInputs == 0 ? 0.0F
                              : routed
                                  ? 0.125F + static_cast<float>(frame) / 16.0F
                                  : static_cast<float>(1.0 - valueAt(frame));
      const auto expected =
          static_cast<float>(valueAt(chunkStart)) + captured + modulation;
      const auto expectedBlocks =
          static_cast<float>(blocks) + static_cast<float>(blocks - 1) / 100.0F;
      if (!withinTolerance(audio.getSample(0, frame) - expected, 1.0e-6F) ||
          (pluginOutputChannels > 1 &&
           !withinTolerance(audio.getSample(1, frame) - expectedBlocks,
                            1.0e-6F))) {
        std::cerr << processor.getName() << " callback " << callback
                  << " length " << frames << " chunk " << block << " frame "
                  << frame << " routed " << routed << ": "
                  << audio.getSample(0, frame) << " expected " << expected
                  << " blocks "
                  << (pluginOutputChannels > 1 ? audio.getSample(1, frame) : 0)
                  << " expected " << expectedBlocks << '\n';
        return false;
      }
    }
  }
  return true;
}

} // namespace test
