#include "ParameterArrayProcessorTests.h"

#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "RunViewHost.h"
#include "TemporaryDirectory.h"
#include "TestAutomationQueue.h"
#include "TestNumeric.h"

#include <fstream>
#include <iostream>

bool exerciseParameterArrayProcessor() {
  using namespace onda::plugin;
  const auto source = testTemporaryRoot() / "parameter-array-processor.onda";
  const auto create = [&] {
    std::ofstream file(source);
    file << "params { levels: f32[" << slotCount + 2 << "] = [";
    for (std::size_t index = 0; index < slotCount + 2; ++index)
      file << (index ? ", " : "") << "0.125";
    file
        << "] {0.0, 1.0} }\nouts { out1 }\n"
           "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
           "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
           "sample { out1 = levels[0] + levels["
        << slotCount - 1 << "] + levels[" << slotCount << "] }\n";
    return file.good();
  };
  const auto renders = [](Processor &processor, float expected) {
    juce::AudioBuffer<float> audio(
        std::max(pluginInputChannels, pluginOutputChannels), 8);
    juce::MidiBuffer midi;
    audio.clear();
    if (!test::processRealtime(processor, audio, midi) ||
        !test::withinTolerance(audio.getSample(0, 0) - expected, 1.0e-6F)) {
      std::cerr << "array slot rendered " << audio.getSample(0, 0)
                << "; expected " << expected << '\n';
      return false;
    }
    return true;
  };
  for (const auto product : {Product::effect, Product::instrument}) {
    if (!create())
      return false;
    Processor processor(product);
    processor.setNonRealtime(true);
    if (!processor.setParamSmoothingSeconds(0.0))
      return false;
    processor.loadFile(juce::File(pathToJuce(source)));
    processor.prepareToPlay(48'000.0, 8);
    test::service(processor);
    if (!processor.workerStatus().active || !renders(processor, 0.375F))
      return false;
    const auto state = makeRunViewState(processor, processor.workerStatus(),
                                        false, {})["state"];
    const auto params = state["params"];
    const auto last = static_cast<int>(slotCount - 1);
    if (params.size() != static_cast<int>(slotCount) ||
        params[0]["array"]["name"].toString() != "levels" ||
        static_cast<int>(params[0]["array"]["length"]) !=
            static_cast<int>(slotCount + 2) ||
        static_cast<int>(params[last]["array"]["index"]) != last ||
        processor.getParameters()[last]->getName(128) !=
            "levels[" + juce::String(last) + "] [Slot " +
                juce::String(last + 1) + "]") {
      std::cerr << "array metadata or DAW parameter names lost scalar slots\n";
      return false;
    }
    processor.setSlotValue(slotCount - 1, 0.5F);
    if (!renders(processor, slotCount == 1 ? 1.125F : 0.75F))
      return false;
    const auto parameterId =
        "slot" + juce::String(static_cast<int>(slotCount)).paddedLeft('0', 2);
    const auto id =
        juce::VST3ClientExtensions::convertJuceParameterId(parameterId);
    const std::array points{AutomationPoint{0, 0.75}};
    TestAutomationQueue queue{points};
    processor.captureHostAutomation(id, queue.queue(), 8);
    if (!renders(processor, slotCount == 1 ? 1.625F : 1.0F))
      return false;
    processor.clearHostAutomation();
    test::service(processor);
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    std::filesystem::remove(source);
    Processor restored(product);
    restored.setNonRealtime(true);
    restored.setStateInformation(saved.getData(),
                                 static_cast<int>(saved.getSize()));
    restored.prepareToPlay(48'000.0, 8);
    if (!restored.workerStatus().usingProjectImage ||
        !renders(restored, slotCount == 1 ? 1.625F : 1.0F))
      return false;
  }
  return true;
}
