#include "AudioFpTests.h"

#include "JucePath.h"
#include "RealtimeAudit.h"
#include "TemporaryDirectory.h"

#include <algorithm>
#include <bit>
#include <iostream>
#include <thread>

bool exerciseAudioThreadFpMode() {
  using namespace onda::plugin;
  const auto fail = [](const char *message) {
    std::cerr << "Audio FP mode: " << message << '\n';
    return false;
  };
  // Restore the test runner's FP mode even if preparation fails.
  juce::ScopedNoDenormals restore;
  juce::FloatVectorOperations::disableDenormalisedNumberSupport(false);
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "audio-thread-fp.onda")};
  if (!source.replaceWithText("params { tiny: f32 = 0.00000000000000000001 }\n"
                              "init { held = tiny * tiny }\n"
                              "outs { out1 }\nsample { out1 = held }\n"))
    return false;
  Processor first(Product::effect), second(Product::effect);
  for (auto *processor : {&first, &second}) {
    processor->loadFile(source);
    processor->prepareToPlay(48'000.0, 8);
    if (!processor->workerStatus().active)
      return fail(processor->workerStatus().message.c_str());
  }
  if (juce::FloatVectorOperations::areDenormalsDisabled())
    return fail("preparation changed the calling host thread");

  // Move the same instances to a new audio thread, including a bypassed first
  // callback. Audit first use as well as subsequent callbacks.
  for (const bool bypassed : {false, true}) {
    bool passed{};
    std::thread audioThread([&] {
      juce::FloatVectorOperations::disableDenormalisedNumberSupport(false);
      juce::AudioBuffer<float> audio(
          std::max(pluginInputChannels, pluginOutputChannels), 8);
      juce::MidiBuffer midi;
      audio.clear();
      if (!test::processRealtime(first, audio, midi, bypassed)) {
        fail("first callback allocated or locked");
        return;
      }
      if (!juce::FloatVectorOperations::areDenormalsDisabled()) {
        fail("first callback did not configure the audio thread");
        return;
      }
      if (!bypassed) {
        // Inspect bits: DAZ could otherwise make a subnormal compare as zero.
        for (int frame = 0; frame < audio.getNumSamples(); ++frame)
          if (std::bit_cast<std::uint32_t>(audio.getSample(0, frame)) != 0U) {
            fail("worker initialization did not flush subnormals");
            return;
          }
      }
      // Each callback must restore the policy if the host cleared it, including
      // when a different instance next executes on this thread.
      for (auto *processor : {&second, &first}) {
        juce::FloatVectorOperations::disableDenormalisedNumberSupport(false);
        if (!test::processRealtime(*processor, audio, midi)) {
          fail("later callback allocated or locked");
          return;
        }
        if (!juce::FloatVectorOperations::areDenormalsDisabled()) {
          fail("later callback did not restore the audio thread policy");
          return;
        }
      }
      passed = true;
    });
    audioThread.join();
    if (!passed)
      return false;
  }
  return !juce::FloatVectorOperations::areDenormalsDisabled();
}
