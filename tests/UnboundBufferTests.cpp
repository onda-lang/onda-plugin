#include "UnboundBufferTests.h"

#include "ControlInputTestSupport.h"
#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "RunViewHost.h"
#include "TemporaryDirectory.h"
#include "TestAudioFile.h"

#include <iostream>

bool exerciseUnboundBuffers() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "unbound-buffer.onda")};
  auto text = "ins {\n" + test::mainAudioInputDeclaration();
  if constexpr (pluginControlInputs > 0)
    text += "cv = 0.25 {0.0, 1.0}\n";
  text += "}\nbuffers { clip: buffer<f32[2]> }\n"
          "params { gain = 0.5 {0.0, 1.0} }\nouts { out1 }\n"
          "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "init { held = 0.0 }\nevent play() { held = 0.125 }\n"
          "sample { out1 = gain + held + clip[0, 0]";
  if constexpr (pluginControlInputs > 0)
    text += " + cv";
  text += " }\n";
  const auto interfaceVisible = [](Processor &processor) {
    test::service(processor);
    const auto status = processor.workerStatus();
    const auto state = makeRunViewState(processor, status, false, {})["state"];
    if (!status.active || state["params"].size() != 1 ||
        state["events"].size() != 1 || state["buffers"].size() != 1 ||
        state["inputControls"].size() != (pluginControlInputs > 0 ? 1 : 0)) {
      std::cerr
          << "unbound buffer hides declared parameters, controls or events: "
          << juce::JSON::toString(state, true) << '\n';
      return false;
    }
    return true;
  };
  const auto renders = [](Processor &processor, float expected) {
    juce::AudioBuffer<float> audio(
        std::max(pluginInputChannels, pluginOutputChannels), 8);
    juce::MidiBuffer midi;
    audio.clear();
    return test::processRealtime(processor, audio, midi) &&
           test::withinTolerance(audio.getSample(0, 0) - expected, 5.0e-4F);
  };
  TemporaryAudioFile clip{"unbound-buffer-clip"};
  if (!clip.write(0.125F, 0.25F))
    return false;
  for (const auto product : {Product::effect, Product::instrument}) {
    if (!source.replaceWithText(text))
      return false;
    Processor processor(product);
    processor.setNonRealtime(true);
    if (!processor.setParamSmoothingSeconds(0.0) ||
        !processor.setInputControlSmoothingSeconds(0.0))
      return false;
    processor.loadFile(source);
    processor.prepareToPlay(48'000.0, 8);
    const auto initial = pluginControlInputs > 0 ? 0.75F : 0.5F;
    if (!interfaceVisible(processor) || !renders(processor, initial))
      return false;
    processor.setSlotValue(0, 0.25F);
    if constexpr (pluginControlInputs > 0)
      processor.setInputControlValue(0, 0.125F);
    const auto edited = pluginControlInputs > 0 ? 0.375F : 0.25F;
    if (!renders(processor, edited) ||
        !processor
             .triggerEvent(processor.workerStatus().engineGeneration, "play",
                           juce::Array<juce::var>{})
             .empty() ||
        !renders(processor, edited + 0.125F))
      return false;
    processor.bindBufferFile("clip", juce::File(pathToJuce(clip.path())));
    processor.prepareToPlay(48'000.0, 8);
    if (!interfaceVisible(processor) || !renders(processor, edited + 0.125F))
      return false;
    processor.clearBuffer("clip");
    processor.prepareToPlay(48'000.0, 8);
    if (!interfaceVisible(processor) || !renders(processor, edited) ||
        processor.workerStatus().buffers[0].waveform)
      return false;
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    if (!source.deleteFile())
      return false;
    Processor restored(product);
    restored.setNonRealtime(true);
    restored.setStateInformation(saved.getData(),
                                 static_cast<int>(saved.getSize()));
    restored.prepareToPlay(48'000.0, 8);
    if (!restored.workerStatus().usingProjectImage ||
        !interfaceVisible(restored) || !renders(restored, edited))
      return false;
  }
  return true;
}
