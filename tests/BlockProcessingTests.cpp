#include "BlockProcessingTests.h"
#include "CallbackProcessingTestSupport.h"
#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "TemporaryDirectory.h"
#include "TestAutomationQueue.h"
#include "TestNumeric.h"

#include <array>
#include <iostream>

bool exerciseCallbackSmoothing() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "callback-smoothing.onda")};
  if (!source.replaceWithText(R"(
outs { out1, out2 }
params { single = 0.0 {0.0, 1.0}; double: f64 = 0.0 {0.0, 1.0} }
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}
block {
  a = single; b = double
  sample { out1 = a; out2 = f32(b) }
}
)"))
    return false;
  for (const auto product : {Product::effect, Product::instrument})
    for (const auto block : {3, 8}) {
      Processor processor(product);
      processor.prepareToPlay(1000.0, 8);
      if (!processor.setPreferredBlockSize(block) ||
          !processor.setParamSmoothingSeconds(0.016))
        return false;
      processor.loadFile(source, false);
      processor.prepareToPlay(1000.0, 8);
      if (!processor.workerStatus().active) {
        std::cerr << processor.workerStatus().message << '\n';
        return false;
      }
      processor.getParameters()[0]->setValueNotifyingHost(0.0F);
      processor.getParameters()[1]->setValueNotifyingHost(0.0F);
      juce::MidiBuffer empty;
      juce::AudioBuffer<float> initial(2, 8);
      if (!test::processRealtime(processor, initial, empty))
        return false;
      processor.setSlotValue(0, 1.0F);
      processor.setSlotValue(1, 1.0F);
      int elapsed{};
      for (const auto frames : {0, 6, 0, 2, 3, 5}) {
        juce::AudioBuffer<float> audio(2, frames);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), frames / 2);
        if (!test::processRealtime(processor, audio, midi))
          return false;
        for (int frame = 0; frame < frames; ++frame) {
          const auto chunkEnd = std::min((frame / block + 1) * block, frames);
          const auto expected = static_cast<float>(elapsed + chunkEnd) / 16.0F;
          for (int channel = 0; channel < 2; ++channel)
            if (!test::withinTolerance(
                    audio.getSample(channel, frame) - expected, 1.0e-6F)) {
              std::cerr << "Short callback smoothing: "
                        << audio.getSample(channel, frame) << " expected "
                        << expected << '\n';
              return false;
            }
        }
        elapsed += frames;
      }
    }
  return true;
}

bool exerciseCallbackProcessing() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "callback-processing.onda")};
  if (!source.replaceWithText(test::callbackProcessingSource()))
    return false;
  const auto parameterId =
      juce::VST3ClientExtensions::convertJuceParameterId("slot01");
  const auto controlId =
      juce::VST3ClientExtensions::convertJuceParameterId("input01");
  for (const auto product : {Product::effect, Product::instrument})
    for (const auto block : {1, 3, 8})
      for (const auto routed : {false, true}) {
        if (routed && pluginControlInputs == 0)
          continue;
        Processor processor(product);
        processor.prepareToPlay(1000.0, 8);
        if (!processor.setPreferredBlockSize(block))
          return false;
        if (routed) {
          auto layout = processor.getBusesLayout();
          layout.inputBuses.set(pluginMainInputBuses,
                                juce::AudioChannelSet::mono());
          if (!processor.setBusesLayout(layout))
            return false;
        }
        processor.loadFile(source, false);
        processor.prepareToPlay(1000.0, 8);
        processor.getParameters()[1]->setValueNotifyingHost(0.0F);
        if (!test::checkCallbackProcessing(
                processor, block, routed,
                [&](juce::AudioBuffer<float> &audio, juce::MidiBuffer &midi,
                    std::span<const AutomationPoint> level,
                    std::span<const AutomationPoint> control) {
                  TestAutomationQueue parameterQueue{level},
                      controlQueue{control};
                  const auto frames = audio.getNumSamples();
                  return test::captureRealtime(processor, parameterId,
                                               parameterQueue.queue(),
                                               frames) &&
                         (pluginControlInputs == 0 ||
                          test::captureRealtime(processor, controlId,
                                                controlQueue.queue(),
                                                frames)) &&
                         test::processRealtime(processor, audio, midi);
                }))
          return false;
      }
  return true;
}

bool exerciseBlockProcessing() {
  using namespace onda::plugin;
  Processor processor(Product::effect);
  processor.prepareToPlay(1000.0, 8);
  if (!processor.canSetBlockSize() || processor.preferredBlockSize() != 0 ||
      processor.ondaBlockSize() != 8 || processor.setPreferredBlockSize(-1) ||
      processor.setPreferredBlockSize(9) || !processor.setPreferredBlockSize(3))
    return false;
  juce::MemoryBlock saved;
  processor.getStateInformation(saved);
  Processor restored(Product::effect);
  restored.setStateInformation(saved.getData(),
                               static_cast<int>(saved.getSize()));
  restored.prepareToPlay(1000.0, 2);
  if (restored.preferredBlockSize() != 3 || restored.ondaBlockSize() != 2)
    return false;
  restored.prepareToPlay(1000.0, 8);
  if (restored.ondaBlockSize() != 3)
    return false;

  const juce::File source{
      pathToJuce(testTemporaryRoot() / "block-processing.onda")};
  if (!source.replaceWithText(R"(
outs { out1, out2 }
params { level = 0.0 {0.0, 1.0}; editor = 0.0 {0.0, 1.0} }
init { blocks = 0; held = 1.0 }
event note_on(id: i32, channel: i32, key: i32, velocity: f32) { held = velocity }
event note_off(id: i32, channel: i32, key: i32, velocity: f32) { held = 0.0 }
event set_held(value: f32) { held = value }
block {
  blocks += 1
  cached = level
  cached_editor = editor
  sample { out1 = cached * held; out2 = f32(blocks) + cached_editor }
}
)"))
    return false;
  processor.loadFile(source);
  processor.prepareToPlay(1000.0, 8);
  if (processor.canSetBlockSize() || processor.setPreferredBlockSize(2))
    return false;
  const auto render = [&](const std::array<float, 8> &left,
                          const std::array<float, 8> &right,
                          juce::MidiBuffer &midi) {
    juce::AudioBuffer<float> audio(2, 8);
    if (!test::processRealtime(processor, audio, midi))
      return false;
    for (int frame = 0; frame < 8; ++frame) {
      const auto index = static_cast<std::size_t>(frame);
      if (!test::withinTolerance(audio.getSample(0, frame) - left[index],
                                 1.0e-6F) ||
          !test::withinTolerance(audio.getSample(1, frame) - right[index],
                                 1.0e-6F)) {
        std::cerr << "Fixed Onda block timing failed at " << frame << " ("
                  << audio.getSample(0, frame) << ", "
                  << audio.getSample(1, frame) << ")\n";
        return false;
      }
    }
    return true;
  };
  const auto slot =
      juce::VST3ClientExtensions::convertJuceParameterId("slot01");
  juce::MidiBuffer empty;
  const std::array rising{AutomationPoint{0, 0.0}, AutomationPoint{7, 1.0}};
  TestAutomationQueue up{rising};
  processor.captureHostAutomation(slot, up.queue(), 8);
  if (!render({0, 0, 0, 3.0F / 7, 3.0F / 7, 3.0F / 7, 6.0F / 7, 6.0F / 7},
              {1, 1, 1, 2, 2, 2, 3, 3}, empty))
    return false;
  const std::array falling{AutomationPoint{7, 0.0}};
  TestAutomationQueue down{falling};
  processor.captureHostAutomation(slot, down.queue(), 8);
  if (!render({.875F, .875F, .875F, .5F, .5F, .5F, .125F, .125F},
              {4, 4, 4, 5, 5, 5, 6, 6}, empty))
    return false;

  // A MIDI event changes sample state at its offset without adding a block.
  processor.getParameters()[0]->setValueNotifyingHost(1.0F);
  juce::MidiBuffer note;
  note.addEvent(juce::MidiMessage::noteOn(1, 60, 0.5F), 3);
  constexpr auto velocity = 64.0F / 127.0F;
  if (!render({1, 1, 1, velocity, velocity, velocity, velocity, velocity},
              {7, 7, 7, 8, 8, 8, 9, 9}, note))
    return false;
  const auto generation = processor.workerStatus().engineGeneration;
  if (!processor
           .triggerEvent(generation, "set_held",
                         juce::var{juce::Array<juce::var>{0.25}})
           .empty() ||
      !render({.25F, .25F, .25F, .25F, .25F, .25F, .25F, .25F},
              {10, 10, 10, 11, 11, 11, 12, 12}, empty))
    return false;

  // Increasing host capacity must resize scratch storage without stranding a
  // fixed-size engine in the deactivated state.
  const auto *oldScratch = ProcessorTestAccess::dryScratchData(processor);
  juce::AudioBuffer<float> oversized(2, 16);
  processor.processBlock(oversized, empty);
  test::service(processor);
  // Preparing larger storage must leave the audio-owned buffer untouched.
  // Smaller callbacks remain eligible throughout preparation.
  if (ProcessorTestAccess::dryScratchData(processor) != oldScratch) {
    std::cerr << "scratch preparation changed audio-owned storage\n";
    return false;
  }
  if (processor.hostBlockSize() != 16 || processor.ondaBlockSize() != 3 ||
      !test::processRealtime(processor, oversized, empty) ||
      !test::withinTolerance(oversized.getSample(0, 0) - .25F, 1.0e-6F))
    return false;
  // A short callback completes its block; reset defaults apply immediately
  // when the next callback starts.
  juce::AudioBuffer<float> partial(2, 1);
  if (!test::processRealtime(processor, partial, empty))
    return false;
  processor.resetParametersToDefaults();
  juce::AudioBuffer<float> settled(2, 5);
  if (!test::processRealtime(processor, settled, empty))
    return false;
  for (int frame = 0; frame < 5; ++frame) {
    if (!test::withinTolerance(settled.getSample(0, frame), 1.0e-6F))
      return false;
  }
  // Editor smoothing advances by each chunk's actual length, independently
  // of the host curve on another slot.
  processor.setSlotValue(1, 1.0F);
  processor.captureHostAutomation(slot, up.queue(), 8);
  if (!render({0, 0, 0, 3.0F / 28, 3.0F / 28, 3.0F / 28, 6.0F / 28, 6.0F / 28},
              {22.1F, 22.1F, 22.1F, 23.2F, 23.2F, 23.2F, 24.0F + 8.0F / 30,
               24.0F + 8.0F / 30},
              empty))
    return false;
  processor.unload();
  return processor.canSetBlockSize() && processor.setPreferredBlockSize(2);
}
