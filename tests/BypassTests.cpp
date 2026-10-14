#include "BypassTests.h"
#include "BypassTestSupport.h"
#include "JucePath.h"
#include "RealtimeAudit.h"
#include "TemporaryDirectory.h"

bool exerciseBypassTransitions() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "bypass-transitions.onda")};
  if (!source.replaceWithText(test::bypassTransitionSource()))
    return false;
  for (const auto product : {Product::effect, Product::instrument}) {
    Processor processor(product);
    processor.loadFile(source, false);
    if constexpr (pluginControlInputs > 0) {
      auto layout = processor.getBusesLayout();
      layout.inputBuses.set(pluginMainInputBuses, juce::AudioChannelSet::mono());
      if (!processor.setBusesLayout(layout))
        return false;
    }
    const auto render = [&](juce::AudioBuffer<float> &audio,
                            juce::MidiBuffer &midi, const bool bypassed) {
      return test::processRealtime(processor, audio, midi, bypassed);
    };
    for (const auto sampleRate : {48'000.0, 96'000.0})
      if (!test::checkBypassTransitions(processor, product == Product::effect,
                                        sampleRate, render))
        return false;
  }
  return true;
}
