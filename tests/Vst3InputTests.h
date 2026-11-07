#pragma once

namespace juce {
class AudioPluginInstance;
class File;
class PluginDescription;
class VST3PluginFormatHeadless;
} // namespace juce
namespace Steinberg::Vst {
class IAudioProcessor;
struct ProcessData;
} // namespace Steinberg::Vst

bool smokeFreshBypassCallbacks(juce::AudioPluginInstance &instance,
                               Steinberg::Vst::IAudioProcessor &processor,
                               Steinberg::Vst::ProcessData &data);
bool smokeVst3MidiInput(juce::VST3PluginFormatHeadless &format,
                        const juce::PluginDescription &description,
                        const juce::File &module);
