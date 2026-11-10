#pragma once
#include <juce_data_structures/juce_data_structures.h>

namespace onda::plugin {

// Appearance is an application preference, independent of saved DSP state.
class UiPreferences {
public:
  UiPreferences() : UiPreferences(options().getDefaultFile()) {}
  explicit UiPreferences(const juce::File &file) : properties_(file, options()) {}

  juce::String theme() const {
    const auto mode = properties_.getValue("theme");
    return validTheme(mode) ? mode : juce::String{};
  }
  bool setTheme(const juce::String &mode) {
    if (!validTheme(mode))
      return false;
    properties_.setValue("theme", mode);
    return properties_.saveIfNeeded();
  }
  static bool validTheme(const juce::String &mode) {
    return mode == "dark" || mode == "light";
  }

private:
  static juce::PropertiesFile::Options options() {
    juce::PropertiesFile::Options result;
    result.applicationName = "onda-ui";
    result.folderName = "Onda";
    result.filenameSuffix = ".settings";
    result.osxLibrarySubFolder = "Application Support";
    result.millisecondsBeforeSaving = -1;
    return result;
  }
  juce::PropertiesFile properties_;
};
} // namespace onda::plugin
