#pragma once

#include "Editor.h"

namespace onda::plugin {

struct EditorTestAccess {
  static void command(Editor &editor, const juce::var &command) {
    editor.handleCommand(command);
  }
};

} // namespace onda::plugin
