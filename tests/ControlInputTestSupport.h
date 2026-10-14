#pragma once

#include "Product.h"
#include "BypassTestSupport.h"
#include "TestNumeric.h"
#include "TestWait.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>

namespace test {

inline juce::String mainAudioInputDeclaration() {
  if constexpr (onda::plugin::pluginInputChannels > 0)
    return "  audio: f32[" + juce::String(onda::plugin::pluginInputChannels) +
           "]\n";
  return {};
}

inline juce::String controlInputSource() {
  using namespace onda::plugin;
  juce::String source{"ins {\n"};
  source += mainAudioInputDeclaration();
  source += "  cv1 = 0.25 {0.0, 1.0}\n";
  if constexpr (pluginControlInputs > 1)
    source += "  cv2 = 0.75 {0.0, 1.0}\n";
  source += "}\nouts { out1";
  if constexpr (pluginOutputChannels > 1)
    source += ", out2";
  source += "}\ninit { blocks = 0; held = 1.0 }\n"
            "event note_on(id: i32, channel: i32, key: i32, velocity: f32) { "
            "held = 0.5 }\n"
            "event note_off(id: i32, channel: i32, key: i32, velocity: f32) { "
            "held = 1.0 }\n"
            "block {\n  blocks += 1\n  sample {\n    out1 = (";
  source += pluginInputChannels > 0 ? "audio[0] * cv1" : "cv1";
  if constexpr (pluginControlInputs > 1)
    source += " + cv2";
  source += ") * held\n";
  if constexpr (pluginOutputChannels > 1)
    source += "    out2 = f32(blocks)\n";
  return source + "  }\n}\n";
}

inline juce::String unusedControlInputSource() {
  juce::String source;
  if constexpr (onda::plugin::pluginInputChannels > 0)
    source = "ins {\n" + mainAudioInputDeclaration() + "}\n";
  source +=
      "outs { out1 }\n"
      "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
      "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
      "sample { out1 = ";
  return source + (onda::plugin::pluginInputChannels > 0 ? "audio[0]" : "0.5") +
         " }\n";
}

inline juce::String allControlInputSource() {
  using namespace onda::plugin;
  auto source = "ins {\n" + mainAudioInputDeclaration();
  for (int index = 0; index < pluginControlInputs; ++index)
    source += "  cv" + juce::String(index + 1) + " = 0.125 {0.0, 1.0}\n";
  source +=
      "}\nouts { out1 }\n"
      "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
      "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n";
  return source + "sample { out1 = cv" + juce::String(pluginControlInputs) +
         " }\n";
}

inline bool disableControlInputBuses(juce::AudioProcessor &processor) {
  using namespace onda::plugin;
  auto layout = processor.getBusesLayout();
  for (int control = 0; control < pluginControlInputs; ++control)
    layout.inputBuses.set(pluginMainInputBuses + control,
                          juce::AudioChannelSet::disabled());
  return processor.setBusesLayout(layout);
}

template <typename Render>
bool checkLastControlInputRouting(juce::AudioProcessor &processor,
                                  Render render) {
  using namespace onda::plugin;
  if constexpr (pluginControlInputs == 0)
    return true;
  const auto fail = [&](const char *message) {
    std::cerr << processor.getName() << ": " << message << '\n';
    return false;
  };
  const auto last = pluginControlInputs - 1;
  processor.releaseResources();
  if (!disableControlInputBuses(processor))
    return fail("could not disable controls before last-bus test");
  processor.prepareToPlay(48'000.0, 8);
  auto *parameter = processor.getParameters()[32 + last];
  if (!waitUntil(
          [&] {
            return parameter->getName(128) == "cv" + juce::String(last + 1);
          },
          [] {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
          }))
    return fail("last control parameter was not mapped");
  juce::MidiBuffer midi;
  const auto checkOutput = [&](const juce::AudioBuffer<float> &audio,
                               const float expected) {
    for (int frame = 0; frame < audio.getNumSamples(); ++frame)
      if (!withinTolerance(audio.getSample(0, frame) - expected, 1.0e-6F)) {
        std::cerr << processor.getName() << " last control sample " << frame
                  << " actual " << audio.getSample(0, frame) << " expected "
                  << expected << " host value " << parameter->getValue()
                  << '\n';
        return false;
      }
    return true;
  };
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  audio.clear();
  if (!render(audio, midi, false) || !checkOutput(audio, 0.125F))
    return fail("last control default failed");
  processor.releaseResources();
  auto layout = processor.getBusesLayout();
  layout.inputBuses.set(pluginMainInputBuses + last,
                        juce::AudioChannelSet::mono());
  if (!processor.setBusesLayout(layout))
    return fail("could not enable last control bus");
  processor.prepareToPlay(48'000.0, 8);
  juce::AudioBuffer<float> routed(
      std::max(processor.getTotalNumInputChannels(), pluginOutputChannels), 8);
  routed.clear();
  std::fill_n(routed.getWritePointer(pluginInputChannels), 8, 0.25F);
  parameter->setValueNotifyingHost(0.75F);
  if (!render(routed, midi, false) || !checkOutput(routed, 0.25F))
    return fail("last control routing failed");
  processor.releaseResources();
  if (!disableControlInputBuses(processor))
    return fail("could not disable last control bus");
  processor.prepareToPlay(48'000.0, 8);
  audio.clear();
  return render(audio, midi, false) && checkOutput(audio, 0.75F);
}

template <typename Render>
bool checkUnusedControlInputs(juce::AudioProcessor &processor, Render render) {
  using namespace onda::plugin;
  processor.releaseResources();
  if (!processor.enableAllBuses())
    return false;
  processor.prepareToPlay(48'000.0, 8);
  for (int callback = 0; callback < 2; ++callback) {
    juce::AudioBuffer<float> audio(
        std::max(processor.getTotalNumInputChannels(), pluginOutputChannels),
        8);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
      std::fill_n(audio.getWritePointer(channel), 8,
                  std::numeric_limits<float>::quiet_NaN());
    for (int channel = 0; channel < pluginInputChannels; ++channel)
      std::fill_n(audio.getWritePointer(channel), 8, 0.5F);
    juce::MidiBuffer midi;
    if (!render(audio, midi, false))
      return false;
    for (int frame = 0; frame < 8; ++frame) {
      if (!withinTolerance(audio.getSample(0, frame) - 0.5F, 1.0e-6))
        return false;
      for (int channel = 1; channel < pluginOutputChannels; ++channel)
        if (!withinTolerance(audio.getSample(channel, frame), 1.0e-6))
          return false;
    }
  }
  processor.releaseResources();
  return true;
}

// Exercise the same scenarios through the processor and the exported VST3.
// The caller chooses how to render, including whether to audit realtime work.
template <typename Render>
bool checkControlInputRouting(juce::AudioProcessor &processor,
                              const bool effect, Render render) {
  using namespace onda::plugin;
  const auto fail = [&](const char *message) {
    std::cerr << processor.getName() << ": " << message << '\n';
    return false;
  };
  if (processor.getBusCount(true) != pluginInputBuses ||
      processor.getTotalNumInputChannels() != pluginInputChannels)
    return fail("control buses must initially be disabled");
  for (int control = 0; control < pluginControlInputs; ++control) {
    const auto *bus = processor.getBus(true, pluginMainInputBuses + control);
    if (bus->isEnabled() ||
        bus->getDefaultLayout() != juce::AudioChannelSet::mono() ||
        (bus->getName() != "Control " + juce::String(control + 1) &&
         bus->getName() != "cv" + juce::String(control + 1)))
      return fail("control bus metadata mismatch");
  }
  if constexpr (pluginControlInputs == 0)
    return true;

  constexpr std::array<float, 8> cv1{
      -0.5F, 0.0F, 0.2F, 1.4F, std::numeric_limits<float>::quiet_NaN(),
      0.6F,  0.8F, 1.0F};
  for (int mask = 0; mask < (pluginControlInputs > 1 ? 4 : 2); ++mask) {
    processor.releaseResources();
    auto layout = processor.getBusesLayout();
    for (int control = 0; control < pluginControlInputs; ++control)
      layout.inputBuses.set(pluginMainInputBuses + control,
                            control < 2 && (mask & (1 << control)) != 0
                                ? juce::AudioChannelSet::mono()
                                : juce::AudioChannelSet::disabled());
    if (!processor.setBusesLayout(layout))
      return fail("could not select control input buses");
    processor.prepareToPlay(48'000.0, 8);
    processor.reset();
    BypassReference bypass;
    for (int callback = 0; callback < 5; ++callback) {
      const auto bypassed = callback == 3;
      juce::AudioBuffer<float> audio(
          std::max(processor.getTotalNumInputChannels(), pluginOutputChannels),
          8);
      audio.clear();
      for (int channel = 0; channel < pluginInputChannels; ++channel)
        std::fill_n(audio.getWritePointer(channel), 8, 0.5F);
      auto hostChannel = pluginInputChannels;
      if ((mask & 1) != 0)
        std::copy(cv1.begin(), cv1.end(), audio.getWritePointer(hostChannel++));
      if ((mask & 2) != 0)
        for (int frame = 0; frame < 8; ++frame)
          audio.setSample(hostChannel, frame,
                          1.0F - static_cast<float>(frame) / 8.0F);
      juce::MidiBuffer midi;
      if (callback == 1)
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), 3);
      bypass.begin(bypassed, 8);
      if (!render(audio, midi, bypassed))
        return fail(
            "processing failed or performed realtime allocations/locks");
      for (int frame = 0; frame < 8; ++frame) {
        const auto value = cv1[static_cast<std::size_t>(frame)];
        const auto first =
            (mask & 1) == 0
                ? 0.25F
                : (std::isnan(value) ? 0.0F : std::clamp(value, 0.0F, 1.0F));
        const auto second =
            pluginControlInputs < 2
                ? 0.0F
                : ((mask & 2) == 0 ? 0.75F
                                   : 1.0F - static_cast<float>(frame) / 8.0F);
        const auto held =
            callback > 1 || (callback == 1 && frame >= 3) ? 0.5F : 1.0F;
        const auto left = bypass.mix(
            ((pluginInputChannels > 0 ? 0.5F : 1.0F) * first + second) * held,
            effect && pluginInputChannels > 0 ? 0.5F : 0.0F, frame);
        if (!withinTolerance(audio.getSample(0, frame) - left, 1.0e-6)) {
          std::cerr << "mask " << mask << ", callback " << callback
                    << ", frame " << frame << ": actual "
                    << audio.getSample(0, frame) << ", expected " << left
                    << '\n';
          return fail("control input mapping, defaults, clamping, or MIDI "
                      "timing failed");
        }
        if constexpr (pluginOutputChannels > 1) {
          const auto right = bypass.mix(
              static_cast<float>(callback * 3 + frame / 3 + 1),
              effect && pluginInputChannels > 1 ? 0.5F : 0.0F, frame);
          if (!withinTolerance(audio.getSample(1, frame) - right, 1.0e-6)) {
            std::cerr << "mask " << mask << ", callback " << callback
                      << ", frame " << frame << ": actual "
                      << audio.getSample(1, frame) << ", expected " << right
                      << '\n';
            return fail("control input aliases, bypass, or logical block "
                        "timing failed");
          }
        }
        for (int channel = 2; channel < pluginOutputChannels; ++channel) {
          const auto expected = bypass.mix(
              0.0F, effect && channel < pluginInputChannels ? 0.5F : 0.0F, frame);
          if (!withinTolerance(audio.getSample(channel, frame) - expected,
                               1.0e-6))
            return fail("unused outputs were not cleared");
        }
      }
      bypass.end(8);
    }
  }
  processor.releaseResources();
  return disableControlInputBuses(processor);
}

} // namespace test
