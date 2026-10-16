#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

// Exercise the OS/browser boundary, including JUCE's separate GTK subprocess.
bool dropNativeFile(juce::WebBrowserComponent &browser, const juce::File &file,
                    juce::Point<int> position = {20, 20});
