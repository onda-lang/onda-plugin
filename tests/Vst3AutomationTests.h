#pragma once

namespace juce {
class AudioPluginInstance;
class File;
class PluginDescription;
class VST3PluginFormatHeadless;
} // namespace juce

bool smokeAutomation(juce::AudioPluginInstance &instance);
bool smokeCallbackProcessing(juce::AudioPluginInstance &instance);

bool smokeControlAutomation(juce::AudioPluginInstance &instance);

bool smokeParameterDefaults(juce::AudioPluginInstance &instance);

bool smokeParameterConversions(juce::VST3PluginFormatHeadless &format,
                                const juce::PluginDescription &description,
                                const juce::File &module);

bool smokeAutomationNotifications(juce::VST3PluginFormatHeadless &format,
                                  const juce::PluginDescription &description,
                                  const juce::File &module);

bool smokeOfflineAutomationNotifications(
    juce::VST3PluginFormatHeadless &format,
    const juce::PluginDescription &description, const juce::File &module);
