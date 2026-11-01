#pragma once

#include "GpuView.h"
#include "NativeGpuSurface.h"

namespace onda::plugin {

struct GpuViewTestAccess {
  static bool initialized(const GpuView &view) { return view.ui_ != nullptr; }
  static auto &input(GpuView &view) { return view.input_; }
  static auto &message(GpuView &view) { return view.message_; }
  static auto &scope(GpuView &view) { return view.scope_; }
  static void sendInput(GpuView &view, const char *json) {
    static_cast<void>(view.input(juce::JSON::parse(json)));
  }
  static float scale(const GpuView &view) { return view.scale_; }
  static auto framebufferSize(const GpuView &view) { return view.framebufferSize_; }
  static auto surfaceWindow(const GpuView &view) {
    return view.surface_ ? view.surface_->handles().window : 0;
  }
  static auto &render(GpuView &view) { return view.render_; }
  static auto &error(GpuView &view) { return view.error_fn_; }
  static bool wakePending(const GpuView &view) { return view.isTimerRunning(); }
  static void expose(GpuView &view) { view.exposed_ = true; }
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
