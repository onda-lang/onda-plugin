#pragma once

#include "ParameterRamp.h"
#include "Product.h"
#include "StateFormat.h"
#include <span>

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

inline juce::File vst3BundleForModule(juce::File module) {
  for (;;) {
    if (module.isDirectory() && module.hasFileExtension("vst3"))
      return module;
    const auto parent = module.getParentDirectory();
    if (parent == module)
      return {};
    module = parent;
  }
}

inline void
setOndaProgram(juce::AudioPluginInstance &instance, const juce::File &source,
               const float normalized = 0.5F, const int blockSamples = 0,
               std::span<const float> controls = {},
               std::span<const onda::plugin::ScalarValue> parameters = {}) {
  juce::MemoryBlock componentState;
  {
    juce::MemoryOutputStream stream(componentState, false);
    stream.writeInt(static_cast<int>(onda::plugin::stateMagic));
    stream.writeInt(onda::plugin::stateVersion);
    stream.writeString(source.getFullPathName());
    stream.writeString({});
    stream.writeInt(0);
    stream.writeInt64(0);
    for (std::size_t slot = 0; slot < 32U; ++slot)
      onda::plugin::writeScalarValue(
          stream, slot < parameters.size()
                      ? parameters[slot]
                      : onda::plugin::ScalarValue{normalized});
    stream.writeInt(480);
    stream.writeInt(720);
    stream.writeBool(false);
    stream.writeString({});
    stream.writeDouble(onda::plugin::defaultParamSmoothingSeconds);
    stream.writeInt(blockSamples);
    stream.writeBool(true);
    stream.writeDouble(onda::plugin::defaultParamSmoothingSeconds);
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(onda::plugin::pluginControlInputs);
         ++index)
      onda::plugin::writeScalarValue(
          stream, index < controls.size() ? controls[index] : 0.5F);
  }
  juce::XmlElement state("VST3PluginState");
  state.createNewChildElement("IComponent")
      ->addTextElement(componentState.toBase64Encoding());
  juce::MemoryBlock hostState;
  juce::AudioProcessor::copyXmlToBinary(state, hostState);
  instance.setStateInformation(hostState.getData(),
                               static_cast<int>(hostState.getSize()));
}
