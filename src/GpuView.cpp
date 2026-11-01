#include "GpuView.h"
#include "NativeGpuSurface.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if JUCE_MAC
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace onda::plugin {
namespace {
float nativeScale(const juce::ComponentPeer &peer) {
#if JUCE_MAC
  // JUCE's platform scale is always 1 on macOS; NSView coordinates are points.
  const auto send = reinterpret_cast<void *(*)(void *, SEL)>(objc_msgSend);
  auto *window = send(peer.getNativeHandle(), sel_registerName("window"));
  if (!window)
    return 1.0f;
  return static_cast<float>(reinterpret_cast<double (*)(void *, SEL)>(
      objc_msgSend)(window, sel_registerName("backingScaleFactor")));
#else
  return static_cast<float>(peer.getPlatformScaleFactor());
#endif
}

juce::File bundledLibrary() {
  juce::File module;
#if JUCE_WINDOWS
  HMODULE handle{};
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&bundledLibrary), &handle)) {
    std::array<wchar_t, 32768> path{};
    const auto size = GetModuleFileNameW(handle, path.data(),
                                         static_cast<DWORD>(path.size()));
    if (size > 0 && size < path.size())
      module = juce::File(juce::String(path.data(), size));
  }
#else
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void *>(&bundledLibrary), &info) &&
      info.dli_fname)
    module = juce::File(juce::String::fromUTF8(info.dli_fname));
#endif
  if (module == juce::File{})
    return {};
  return module.getParentDirectory()
      .getSiblingFile("Resources")
      .getChildFile("onda_ui")
      .getChildFile(ONDA_GPU_UI_LIBRARY_NAME);
}

juce::var event(const char *type) {
  juce::var value{new juce::DynamicObject};
  value.getDynamicObject()->setProperty("type", type);
  return value;
}
juce::var pointer(const char *type, const juce::MouseEvent &mouse) {
  auto value = event(type);
  auto &object = *value.getDynamicObject();
  object.setProperty("x", mouse.position.x);
  object.setProperty("y", mouse.position.y);
  object.setProperty("fine", mouse.mods.isShiftDown());
  object.setProperty("clicks", mouse.getNumberOfClicks());
  object.setProperty("id", mouse.source.getIndex());
  return value;
}
} // namespace

GpuView::GpuView(Command command, std::function<void()> ready)
    : ComponentMovementWatcher(this), command_(std::move(command)),
      ready_(std::move(ready)), scaleNotifier_(this, [this](float) {
        exposed_ = true;
        triggerAsyncUpdate();
      }) {
  setWantsKeyboardFocus(true);
  setOpaque(false);
}
GpuView::~GpuView() { release(); }
void GpuView::release() {
  stopTimer();
  cancelPendingUpdate();
  if (ui_)
    input(event("focusLost"));
  if (ui_)
    destroy_(std::exchange(ui_, nullptr));
  surface_.reset();
  peerId_ = 0;
  framebufferSize_ = {};
  redrawRetryMs_ = 0;
  cancelPendingUpdate();
}
bool GpuView::initialize() {
  if (!getPeer() || !isShowing() || getLocalBounds().isEmpty() ||
      !error_.isEmpty())
    return false;
  if (!create_) {
    const auto path = juce::SystemStats::getEnvironmentVariable(
        "ONDA_GPU_UI_LIBRARY", bundledLibrary().getFullPathName());
    if (!library_.open(path)) {
      error_ = "Could not load GPU UI library: " + path;
      return false;
    }
#define ONDA_LOAD(member, name)                                                \
  member = reinterpret_cast<decltype(member)>(library_.getFunction(name));     \
  if (!member) {                                                               \
    error_ = "GPU UI library is missing " name;                                \
    return false;                                                              \
  }
    ONDA_LOAD(create_, "onda_ui_create")
    ONDA_LOAD(destroy_, "onda_ui_destroy")
    ONDA_LOAD(resize_, "onda_ui_resize")
    ONDA_LOAD(message_, "onda_ui_message")
    ONDA_LOAD(input_, "onda_ui_input")
    ONDA_LOAD(cursor_, "onda_ui_cursor")
    ONDA_LOAD(render_, "onda_ui_render")
    ONDA_LOAD(update_, "onda_ui_update")
    ONDA_LOAD(wake_delay_, "onda_ui_wake_delay_ms")
    ONDA_LOAD(meter_, "onda_ui_meter")
    ONDA_LOAD(meter_visible_, "onda_ui_meter_visible")
    ONDA_LOAD(scope_, "onda_ui_scope")
    ONDA_LOAD(scope_visible_, "onda_ui_scope_visible")
    ONDA_LOAD(commands_, "onda_ui_commands")
    ONDA_LOAD(error_fn_, "onda_ui_error")
    ONDA_LOAD(text_edit_, "onda_ui_text_edit")
    ONDA_LOAD(text_bounds_, "onda_ui_text_bounds")
    ONDA_LOAD(text_hit_, "onda_ui_text_hit")
#undef ONDA_LOAD
  }
  surface_ = NativeGpuSurface::create(
      *getPeer(),
      [this] {
        exposed_ = true;
        triggerAsyncUpdate();
      },
      error_);
  if (!surface_)
    return false;
  peerId_ = getPeer()->getUniqueID();
  updateSurface();
  const auto handles = surface_->handles();
  std::array<char, 2048> error{};
  ui_ = create_(handles.platform, handles.window, handles.display,
                static_cast<uint32_t>(framebufferSize_.x),
                static_cast<uint32_t>(framebufferSize_.y), scale_, error.data(),
                error.size());
  if (!ui_) {
    error_ = error[0] ? juce::String::fromUTF8(error.data())
                      : "GPU UI initialization failed";
    release();
    return false;
  }
  ready_();
  return true;
}
void GpuView::tick() {
  cancelPendingUpdate();
  stopTimer();
  if (ui_ && (!getPeer() || getPeer()->getUniqueID() != peerId_)) {
    release();
  }
  if (!isShowing()) {
    if (surface_)
      surface_->setVisible(false);
    return;
  }
  if (!ui_ && !initialize())
    return;
  updateSurface();
  if (exposed_) {
    exposed_ = false;
    redrawRetryMs_ = 0;
    resize_(ui_, static_cast<uint32_t>(framebufferSize_.x),
            static_cast<uint32_t>(framebufferSize_.y), scale_);
  }
  const auto redraw = update_(ui_);
  drain();
  if (redraw < 0) {
    error_ = juce::String::fromUTF8(error_fn_(ui_));
    if (error_.isEmpty())
      error_ = "GPU UI update failed; reopen the editor";
  }
  if (!error_.isEmpty()) {
    release();
    return;
  }
  if (redraw > 0 && juce::Time::getMillisecondCounterHiRes() >= redrawRetryMs_)
    draw();
  armWake();
}
void GpuView::timerCallback() {
  stopTimer();
  triggerAsyncUpdate();
}
void GpuView::armWake() {
  if (!ui_ || !isShowing())
    return;
  auto delay = wake_delay_(ui_);
  if (redrawRetryMs_ > 0) {
    const auto retry = std::max(
        0.0, redrawRetryMs_ - juce::Time::getMillisecondCounterHiRes());
    delay = delay < 0 ? retry : std::min(delay, retry);
  }
  if (delay >= 0)
    startTimer(static_cast<int>(
        std::clamp(std::ceil(delay), 1.0,
                   static_cast<double>(std::numeric_limits<int>::max()))));
}
void GpuView::updateSurface() {
  if (!surface_ || !getPeer())
    return;
  const auto scale = nativeScale(*getPeer());
  const auto size =
      surface_->setBounds(getPeer()->getAreaCoveredBy(*this), scale);
  if (size != framebufferSize_ || !juce::approximatelyEqual(scale, scale_)) {
    framebufferSize_ = size;
    scale_ = scale;
    exposed_ = true;
  }
}
void GpuView::draw() {
  if (!ui_ || drawing_ || !isShowing())
    return;
  const juce::ScopedValueSetter<bool> drawing(drawing_, true);
  if (getLocalBounds().isEmpty()) {
    surface_->setVisible(false);
    drain();
    return;
  }
  const auto result = render_(ui_);
  if (result < 0) {
    error_ = juce::String::fromUTF8(error_fn_(ui_));
    if (error_.isEmpty())
      error_ = "GPU UI rendering failed; reopen the editor";
  }
  if (!error_.isEmpty()) {
    release();
    return;
  }
  if (result > 0)
    surface_->setVisible(true);
  // An unavailable swapchain must retry even if no input or audio arrives.
  redrawRetryMs_ =
      result == 0 ? juce::Time::getMillisecondCounterHiRes() + 1000.0 / 30.0
                  : 0.0;
  drain();
}
void GpuView::componentMovedOrResized(bool, bool) {
  if (ui_) {
    if (getLocalBounds().isEmpty())
      suspend();
    tick();
  }
}
void GpuView::componentPeerChanged() {
  release();
  triggerAsyncUpdate();
}
void GpuView::componentVisibilityChanged() {
  if (!isShowing()) {
    suspend();
    if (surface_)
      surface_->setVisible(false);
  }
  exposed_ = true;
  triggerAsyncUpdate();
}
void GpuView::message(const juce::var &value) {
  if (!ui_)
    return;
  const auto json = juce::JSON::toString(value, true);
  if (!message_(ui_, json.toRawUTF8()))
    error_ = juce::String::fromUTF8(error_fn_(ui_));
  drain();
  triggerAsyncUpdate();
}
void GpuView::meter(int channels, const float *peaks, std::size_t count) {
  if (ui_) {
    meter_(ui_, static_cast<uint32_t>(channels), peaks, count);
    triggerAsyncUpdate();
  }
}

void GpuView::scope(int channels, const std::vector<float> &samples) {
  if (ui_) {
    scope_(ui_, static_cast<uint32_t>(channels), samples.data(),
           samples.size());
    triggerAsyncUpdate();
  }
}
bool GpuView::input(juce::var value) {
  if (!ui_)
    return false;
  const auto json = juce::JSON::toString(value, true);
  const auto result = input_(ui_, json.toRawUTF8());
  if (result < 0)
    error_ = juce::String::fromUTF8(error_fn_(ui_));
  drain();
  triggerAsyncUpdate();
  return result > 0;
}
void GpuView::drain() {
  if (!ui_)
    return;
  // Schema replacement can cancel the draft without a native IME callback.
  if (composing_ && !isTextInputActive())
    composing_ = false;
  if (auto *peer = getPeer())
    peer->refreshTextInputTarget();
  const auto cursorName = juce::String::fromUTF8(cursor_(ui_));
  setMouseCursor(
      cursorName == "pointer"     ? juce::MouseCursor::PointingHandCursor
      : cursorName == "text"      ? juce::MouseCursor::IBeamCursor
      : cursorName == "ew-resize" ? juce::MouseCursor::LeftRightResizeCursor
      : cursorName == "ns-resize" ? juce::MouseCursor::UpDownResizeCursor
                                  : juce::MouseCursor::NormalCursor);
  // Parse before invoking callbacks; callbacks may publish state recursively.
  const auto values = juce::JSON::parse(juce::String::fromUTF8(commands_(ui_)));
  if (const auto *array = values.getArray())
    for (const auto &value : *array) {
      if (value["type"].toString() == "copyText")
        juce::SystemClipboard::copyTextToClipboard(value["text"].toString());
      else
        command_(value);
    }
}
void GpuView::mouseDown(const juce::MouseEvent &mouse) {
  if (!mouse.mods.isLeftButtonDown())
    return;
  grabKeyboardFocus();
  input(pointer(mouse.source.isTouch() ? "touchStart" : "pointerDown", mouse));
}
void GpuView::mouseDrag(const juce::MouseEvent &mouse) {
  if (!mouse.mods.isLeftButtonDown())
    return;
  input(pointer(mouse.source.isTouch() ? "touchMove" : "pointerMove", mouse));
}
void GpuView::mouseMove(const juce::MouseEvent &mouse) {
  input(pointer("pointerMove", mouse));
}
void GpuView::mouseExit(const juce::MouseEvent &) {
  input(event("pointerLeave"));
}
void GpuView::mouseUp(const juce::MouseEvent &mouse) {
  if (mouse.mods.isLeftButtonDown())
    input(pointer(mouse.source.isTouch() ? "touchEnd" : "pointerUp", mouse));
}
void GpuView::mouseWheelMove(const juce::MouseEvent &mouse,
                             const juce::MouseWheelDetails &wheel) {
  input(pointer("pointerMove", mouse));
  const bool horizontal = mouse.mods.isShiftDown() ||
                          std::abs(wheel.deltaX) > std::abs(wheel.deltaY);
  auto value = event(horizontal ? "horizontalScroll" : "scroll");
  value.getDynamicObject()->setProperty(
      "delta",
      -(horizontal && !mouse.mods.isShiftDown() ? wheel.deltaX : wheel.deltaY) *
          200.0f);
  input(value);
}
bool GpuView::keyPressed(const juce::KeyPress &key) {
  if (composing_ && !key.getModifiers().isCommandDown())
    return false;
  const char *name = nullptr;
  const auto code = key.getKeyCode();
  const auto modifiers = key.getModifiers();
  if (modifiers.isCommandDown()) {
    if (code == 'A' || code == 'a')
      name = "SelectAll";
    else if (code == 'C' || code == 'c')
      name = "Copy";
    else if (code == 'X' || code == 'x')
      name = "Cut";
    else if ((code == 'V' || code == 'v') && isTextInputActive()) {
      auto value = event("text");
      value.getDynamicObject()->setProperty(
          "text", juce::SystemClipboard::getTextFromClipboard());
      return input(value);
    } else {
      return false;
    }
  } else if (code == juce::KeyPress::returnKey)
    name = "Enter";
  else if (code == juce::KeyPress::escapeKey)
    name = "Escape";
  else if (code == juce::KeyPress::tabKey)
    name = "Tab";
  else if (code == juce::KeyPress::backspaceKey)
    name = "Backspace";
  else if (code == juce::KeyPress::deleteKey)
    name = "Delete";
  else if (code == juce::KeyPress::leftKey)
    name = "ArrowLeft";
  else if (code == juce::KeyPress::rightKey)
    name = "ArrowRight";
  else if (code == juce::KeyPress::upKey)
    name = "ArrowUp";
  else if (code == juce::KeyPress::downKey)
    name = "ArrowDown";
  else if (code == juce::KeyPress::homeKey)
    name = "Home";
  else if (code == juce::KeyPress::endKey)
    name = "End";
  if (name) {
    auto value = event("key");
    value.getDynamicObject()->setProperty("key", name);
    value.getDynamicObject()->setProperty("shift",
                                          key.getModifiers().isShiftDown());
    return input(value);
  }
  if (const auto c = key.getTextCharacter();
      c >= 32 && !key.getModifiers().isCommandDown()) {
    auto value = event("text");
    value.getDynamicObject()->setProperty("text",
                                          juce::String::charToString(c));
    return input(value);
  }
  return false;
}
void GpuView::suspend() {
  composing_ = false;
  input(event("focusLost"));
}
void GpuView::focusLost(FocusChangeType) { suspend(); }
} // namespace onda::plugin
