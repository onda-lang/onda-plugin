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
    const juce::File arrayFile{
        pathToJuce(testTemporaryRoot() / "control-array-default.onda")};
    for (int count = 1; count <= std::min(2, pluginControlInputs); ++count) {
      // One array spans the main audio and control slots. Each control must
      // retain its own default, even when that default lies outside [0, 1].
      const auto length = pluginInputChannels + count;
      auto arraySource = "ins { cv: f32[" + juce::String(length) + "] = [";
      for (int element = 0; element < length; ++element)
        arraySource += (element == 0 ? "" : ", ") +
                       juce::String(static_cast<double>(element) + 0.25);
      arraySource += "] }\nouts { out1 }\nsample { out1 = cv[" +
                     juce::String(length - 1) + "] }\n";
      if (!arrayFile.replaceWithText(arraySource))
        return false;
      const auto built =
          PreparedEngine::build(pathFromJuce(arrayFile.getFullPathName()),
                                Product::effect, 48'000.0, 3);
      if (!built.engine || built.engine->controlMappings().size() !=
                               static_cast<std::size_t>(count))
        return false;
      for (int control = 0; control < count; ++control) {
        const auto element = pluginInputChannels + control;
        const auto &mapping = built.engine->controlMappings()[
            static_cast<std::size_t>(control)];
        if (mapping.name != "cv[" + std::to_string(element) + "]" ||
            mapping.defaultPlain !=
                ScalarPlain{static_cast<double>(element) + 0.25})
          return false;
      }
      std::array<std::array<float, 3>, pluginOutputChannels> samples{};
      std::array<float *, pluginOutputChannels> outputs{};
      for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        outputs[channel] = samples[channel].data();
      ParameterState slots;
      if (!built.engine->process(nullptr, outputs.data(), 3, {}, slots) ||
          !std::ranges::all_of(samples[0], [length](const float value) {
            return test::withinTolerance(
                value - (static_cast<float>(length - 1) + 0.25F), 1.0e-6F);
          }))
        return false;
    }
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
