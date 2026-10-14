#include "ControlInputTests.h"
#include "ControlInputTestSupport.h"
#include "JucePath.h"
#include "RealtimeAudit.h"
#include "TemporaryDirectory.h"

bool exerciseControlInputs() {
  using namespace onda::plugin;
  Processor processor(Product::effect);
  if constexpr (pluginControlInputs > 0) {
    auto invalid = processor.getBusesLayout();
    invalid.inputBuses.set(pluginMainInputBuses,
                           juce::AudioChannelSet::stereo());
    if (processor.isBusesLayoutSupported(invalid))
      return false;
    invalid = processor.getBusesLayout();
    invalid.inputBuses.remove(pluginMainInputBuses);
    if (processor.isBusesLayoutSupported(invalid))
      return false;
    processor.prepareToPlay(48'000.0, 8);
    if (!processor.setPreferredBlockSize(3))
      return false;
    const juce::File source{
        pathToJuce(testTemporaryRoot() / "control-inputs.onda")};
    if (!source.replaceWithText(test::controlInputSource()))
      return false;
    const auto inputPrefix = "ins {\n" + test::mainAudioInputDeclaration();
    juce::String arraySource{inputPrefix};
    arraySource +=
        "  cv: f32[1] = [0.5]\n}\nouts { out1 }\nsample { out1 = cv[0] }\n";
    const juce::File arrayFile{
        pathToJuce(testTemporaryRoot() / "control-array-default.onda")};
    if (!arrayFile.replaceWithText(arraySource))
      return false;
    const auto rejected =
        PreparedEngine::build(pathFromJuce(arrayFile.getFullPathName()),
                              Product::effect, 48'000.0, 3);
    if (rejected.engine ||
        rejected.diagnostic.message.find("finite scalars") == std::string::npos)
      return false;
    const juce::File invalidTypeFile{
        pathToJuce(testTemporaryRoot() / "control-invalid-type.onda")};
    for (const auto *type : {"f64", "f64[1]", "i32", "bool"}) {
      if (!invalidTypeFile.replaceWithText(
              inputPrefix + "  cv: " + type +
              "\n}\nouts { out1 }\nsample { out1 = 0.0 }\n"))
        return false;
      const auto invalidType =
          PreparedEngine::build(pathFromJuce(invalidTypeFile.getFullPathName()),
                                Product::effect, 48'000.0, 3);
      if (invalidType.engine || invalidType.diagnostic.message.find(
                                    "f32 channels") == std::string::npos)
        return false;
    }
    processor.loadFile(source);
  }
  const auto render = [&](juce::AudioBuffer<float> &audio,
                          juce::MidiBuffer &midi, const bool bypassed) {
    return test::processRealtime(processor, audio, midi, bypassed);
  };
  if (!test::checkControlInputRouting(processor, true, render))
    return false;
  if constexpr (pluginControlInputs > 0) {
    const juce::File all{
        pathToJuce(testTemporaryRoot() / "all-control-inputs.onda")};
    if (!all.replaceWithText(test::allControlInputSource()))
      return false;
    processor.loadFile(all);
    if (!test::checkLastControlInputRouting(processor, render))
      return false;
  }
  const juce::File unused{
      pathToJuce(testTemporaryRoot() / "unused-control-inputs.onda")};
  if (!unused.replaceWithText(test::unusedControlInputSource()))
    return false;
  processor.loadFile(unused);
  return test::checkUnusedControlInputs(processor, render);
}
