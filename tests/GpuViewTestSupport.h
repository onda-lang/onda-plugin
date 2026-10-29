#pragma once

#include "GpuView.h"

namespace onda::plugin {

struct GpuViewTestAccess {
  static auto &input(GpuView &view) { return view.input_; }
  static auto &message(GpuView &view) { return view.message_; }
  static auto &scope(GpuView &view) { return view.scope_; }
  static float scale(const GpuView &view) { return view.scale_; }
  static void connectInput(GpuView &view, decltype(&onda_ui_input) input) {
    view.ui_ = reinterpret_cast<OndaUi *>(&view);
    view.input_ = input;
    view.destroy_ = [](OndaUi *) {};
    view.cursor_ = [](OndaUi *) { return "default"; };
    view.commands_ = [](OndaUi *) { return "[]"; };
    view.error_fn_ = [](const OndaUi *) { return "input error"; };
    view.text_edit_ = [](OndaUi *) { return "null"; };
  }
};

} // namespace onda::plugin
