#pragma once

#include "Product.h"
#include "TestNumeric.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <iostream>

namespace test {

// Independent reference: evaluate the elapsed fraction with a weighted sum,
// rather than accumulating JUCE's smoothing increments.
class BypassReference {
public:
  explicit BypassReference(const double sampleRate = 48'000.0)
      : duration_(static_cast<int>(std::floor(sampleRate * 0.005))),
        elapsed_(duration_) {}

  void begin(const bool bypassed, const int frames) {
    if (frames == 0)
      return;
    const long double target = bypassed ? 0.0L : 1.0L;
    if (!started_) {
      start_ = target_ = target;
      elapsed_ = duration_;
      started_ = true;
    } else if (std::is_neq(target <=> target_)) {
      start_ = gain(0);
      target_ = target;
      elapsed_ = 0;
    }
  }

  float mix(const float wet, const float dry, const int frame) const {
    const auto amount = gain(frame + 1);
    return static_cast<float>(amount * wet + (1.0L - amount) * dry);
  }

  void end(const int frames) { elapsed_ = std::min(duration_, elapsed_ + frames); }

private:
  long double gain(const int samples) const {
    const auto elapsed = std::min(duration_, elapsed_ + samples);
    if (elapsed == duration_)
      return target_;
    const auto fraction = static_cast<long double>(elapsed) / duration_;
    return fraction * target_ + (1.0L - fraction) * start_;
  }

  long double start_{1.0L}, target_{1.0L};
  int duration_, elapsed_;
  bool started_{};
};

inline juce::String bypassTransitionSource() {
  using namespace onda::plugin;
  juce::String source{"outs { out1"};
  if constexpr (pluginOutputChannels > 1)
    source += ", out2";
  source += "}\n"
            "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
            "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
            "sample { out1 = 1.0";
  if constexpr (pluginOutputChannels > 1)
    source += "; out2 = -0.5";
  return source + " }\n";
}

template <typename Render>
bool checkBypassTransitions(juce::AudioProcessor &processor, const bool effect,
                            const double sampleRate, Render render) {
  using namespace onda::plugin;
  processor.prepareToPlay(sampleRate, 256);
  BypassReference reference(sampleRate);
  struct Callback {
    bool bypassed;
    int frames;
  };
  // Initial bypass, rapid reversals, empty callbacks, variable lengths, and
  // both settled endpoints. The 96 kHz case spans twice as many fade samples.
  constexpr std::array callbacks{
      Callback{true, 0},   Callback{true, 7},    Callback{true, 0},
      Callback{false, 11}, Callback{true, 5},    Callback{false, 0},
      Callback{false, 0},  Callback{false, 13},  Callback{true, 31},
      Callback{true, 0},   Callback{true, 43},   Callback{true, 151},
      Callback{true, 256}, Callback{false, 17},  Callback{false, 0},
      Callback{false, 67}, Callback{false, 256}, Callback{false, 256},
      Callback{true, 256}, Callback{true, 256},  Callback{false, 256}};
  for (const auto callback : callbacks) {
    juce::AudioBuffer<float> audio(
        std::max(processor.getTotalNumInputChannels(), pluginOutputChannels),
        callback.frames);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
      std::fill_n(audio.getWritePointer(channel), callback.frames,
                  channel < pluginInputChannels ? 0.25F : 2.0F);
    juce::MidiBuffer midi;
    if (callback.frames > 0) {
      midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), 0);
      midi.addEvent(juce::MidiMessage::noteOff(1, 60), callback.frames / 2);
    }
    reference.begin(callback.bypassed, callback.frames);
    if (!render(audio, midi, callback.bypassed))
      return false;
    for (int channel = 0; channel < pluginOutputChannels; ++channel) {
      const auto wet = channel == 0 ? 1.0F : channel == 1 ? -0.5F : 0.0F;
      const auto dry =
          effect && channel < pluginPassthroughChannels ? 0.25F : 0.0F;
      for (int frame = 0; frame < callback.frames; ++frame) {
        const auto expected = reference.mix(wet, dry, frame);
        if (!withinTolerance(audio.getSample(channel, frame) - expected,
                             1.0e-6F)) {
          std::cerr << processor.getName() << " bypass " << callback.bypassed
                    << " at " << sampleRate << " Hz, length " << callback.frames
                    << ", channel " << channel << ", frame " << frame
                    << ": actual " << audio.getSample(channel, frame)
                    << ", expected " << expected << '\n';
          return false;
        }
      }
    }
    reference.end(callback.frames);
  }
  processor.releaseResources();
  return true;
}

} // namespace test
