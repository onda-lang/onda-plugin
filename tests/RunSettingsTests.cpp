#include "RunSettingsTests.h"

#include "EditorTestSupport.h"
#include "JucePath.h"
#include "Processor.h"
#include "ProcessorTestSupport.h"
#include "RunViewHost.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"

#include <algorithm>
#include <iostream>

bool exerciseFastMath() {
  using namespace onda::plugin;
  const juce::File source{pathToJuce(testTemporaryRoot() / "fast-math.onda")};
  const auto writeSource = [&](int increment) {
    return source.replaceWithText(
        "outs { out1 }\nparams { value = 100000000000000000000.0 }\nsample { "
        "out1 = (value + " +
        juce::String(increment) + ".0) - value }\n");
  };
  const auto renders = [](Processor &processor, float expected) {
    juce::AudioBuffer<float> audio(
        std::max(pluginInputChannels, pluginOutputChannels), 64);
    juce::MidiBuffer midi;
    audio.clear();
    processor.processBlock(audio, midi);
    const auto actual = audio.getSample(0, 0);
    if (!test::withinTolerance(actual - expected, 1.0e-6F))
      std::cerr << "Fast Math sample: " << actual << ", expected " << expected
                << '\n';
    return test::withinTolerance(actual - expected, 1.0e-6F);
  };

  for (const auto enabled : {false, true}) {
    if (!writeSource(1))
      return false;
    Processor processor(Product::effect);
    processor.setNonRealtime(true);
    if (processor.fastMath()) {
      std::cerr << "Empty plugin instances must default to strict math\n";
      return false;
    }
    {
      Editor editor(processor);
      juce::var command{new juce::DynamicObject};
      command.getDynamicObject()->setProperty("type", "setFastMath");
      command.getDynamicObject()->setProperty("enabled", enabled);
      EditorTestAccess::command(editor, command);
    }
    const auto message =
        makeRunViewState(processor, processor.workerStatus(), false, {});
    const auto state = message.getProperty("state", {});
    if (processor.fastMath() != enabled ||
        !static_cast<bool>(state.getProperty("supportsFastMath", false)) ||
        static_cast<bool>(state.getProperty("fastMath", false)) != enabled)
      return false;
    processor.loadFile(source);
    processor.prepareToPlay(48'000.0, 64);
    const auto expected = enabled ? 1.0F : 0.0F;
    if (!processor.workerStatus().active || !renders(processor, expected)) {
      std::cerr << "Fast Math " << enabled
                << " disk compilation: " << processor.workerStatus().message
                << '\n';
      return false;
    }
    if (processor.setFastMath(!enabled) || processor.fastMath() != enabled)
      return false;

    // Automatic recompilation and host specialization retain the option.
    if (!writeSource(2))
      return false;
    const auto generation = processor.workerStatus().engineGeneration;
    if (!test::waitForMessage([&] {
          return processor.workerStatus().engineGeneration > generation &&
                 renders(processor, enabled ? 2.0F : 0.0F);
        }))
      return false;
    processor.prepareToPlay(44'100.0, 64);
    if (!renders(processor, enabled ? 2.0F : 0.0F))
      return false;

    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    if (!source.deleteFile())
      return false;
    Processor restored(Product::effect);
    restored.setNonRealtime(true);
    restored.setStateInformation(saved.getData(),
                                 static_cast<int>(saved.getSize()));
    restored.prepareToPlay(48'000.0, 64);
    if (restored.fastMath() != enabled ||
        !restored.workerStatus().usingProjectImage ||
        !renders(restored, enabled ? 2.0F : 0.0F)) {
      std::cerr << "Fast Math did not survive saved-project fallback\n";
      return false;
    }
    restored.unload();
    if (!restored.setFastMath(!enabled))
      return false;
  }
  return true;
}
