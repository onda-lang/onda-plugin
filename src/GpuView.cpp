#include "GpuView.h"
#include <array>
#include <cmath>
#include <utility>
#if JUCE_WINDOWS
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
    : command_(std::move(command)), ready_(std::move(ready)) {
  setWantsKeyboardFocus(true);
  setOpaque(false);
}
GpuView::~GpuView() { release(); }
void GpuView::release() {
  if (ui_)
    input(event("focusLost"));
  if (ui_)
    destroy_(std::exchange(ui_, nullptr));
#if JUCE_WINDOWS
  if (previousRenderingEngine_ >= 0) {
    // The view may already belong to another peer, or the old peer may be gone.
    for (int index = 0; index < juce::ComponentPeer::getNumPeers(); ++index) {
      auto *peer = juce::ComponentPeer::getPeer(index);
      if (peer->getUniqueID() == renderingPeerId_) {
        peer->setCurrentRenderingEngine(previousRenderingEngine_);
        break;
      }
    }
    previousRenderingEngine_ = -1;
  }
#endif
#if JUCE_LINUX
  if (display_) {
    if (auto close = reinterpret_cast<int (*)(void *)>(
            x11_.getFunction("XCloseDisplay")))
      close(display_);
    display_ = nullptr;
  }
#endif
  peer_ = nullptr;
}
bool GpuView::initialize() {
  if (!getPeer() || !isShowing() || !error_.isEmpty())
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
    ONDA_LOAD(scope_, "onda_ui_scope")
    ONDA_LOAD(scope_visible_, "onda_ui_scope_visible")
    ONDA_LOAD(commands_, "onda_ui_commands")
    ONDA_LOAD(error_fn_, "onda_ui_error")
    ONDA_LOAD(text_edit_, "onda_ui_text_edit")
    ONDA_LOAD(text_bounds_, "onda_ui_text_bounds")
    ONDA_LOAD(text_hit_, "onda_ui_text_hit")
#undef ONDA_LOAD
  }
  peer_ = getPeer()->getNativeHandle();
  unsigned platform{};
#if JUCE_LINUX
  platform = 1;
  if (!x11_.open("libX11.so.6")) {
    error_ = "Could not load X11 for GPU UI";
    return false;
  }
  auto open = reinterpret_cast<void *(*)(const char *)>(
      x11_.getFunction("XOpenDisplay"));
  display_ = open ? open(nullptr) : nullptr;
#elif JUCE_WINDOWS
  platform = 2;
  // JUCE's Direct2D composition layer covers wgpu's HWND swap chain. Keep
  // JUCE's background paint on GDI while Rust owns GPU presentation.
  renderingPeerId_ = getPeer()->getUniqueID();
  previousRenderingEngine_ = getPeer()->getCurrentRenderingEngine();
  getPeer()->setCurrentRenderingEngine(0);
#elif JUCE_MAC
  platform = 3;
#endif
  scale_ = nativeScale(*getPeer());
  std::array<char, 2048> error{};
  ui_ = create_(platform, reinterpret_cast<uintptr_t>(peer_),
                reinterpret_cast<uintptr_t>(display_),
                static_cast<uint32_t>(std::lround(getWidth() * scale_)),
                static_cast<uint32_t>(std::lround(getHeight() * scale_)),
                scale_, error.data(), error.size());
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
  if (ui_ && (!getPeer() || getPeer()->getNativeHandle() != peer_)) {
    input(event("focusLost"));
    release();
  }
  if (!isShowing()) {
    if (wasShowing_) {
      input(event("focusLost"));
    }
    wasShowing_ = false;
    return;
  }
  wasShowing_ = true;
  if (!ui_ && !initialize())
    return;
  const auto scale = nativeScale(*getPeer());
  if (!juce::approximatelyEqual(scale, scale_)) {
    scale_ = scale;
    resized();
  }
  if (exposed_) {
    resized();
    exposed_ = false;
  }
  if (render_(ui_) < 0)
    error_ = juce::String::fromUTF8(error_fn_(ui_));
  drain();
}
void GpuView::resized() {
  if (ui_)
    resize_(ui_, static_cast<uint32_t>(std::lround(getWidth() * scale_)),
            static_cast<uint32_t>(std::lround(getHeight() * scale_)), scale_);
}
void GpuView::paint(juce::Graphics &) { exposed_ = true; }
void GpuView::message(const juce::var &value) {
  if (!ui_)
    return;
  const auto json = juce::JSON::toString(value, true);
  if (!message_(ui_, json.toRawUTF8()))
    error_ = juce::String::fromUTF8(error_fn_(ui_));
  drain();
}
void GpuView::scope(int channels, const std::vector<float> &samples) {
  if (ui_)
    scope_(ui_, static_cast<uint32_t>(channels), samples.data(),
           samples.size());
}
bool GpuView::input(juce::var value) {
  if (!ui_)
    return false;
  const auto json = juce::JSON::toString(value, true);
  const auto result = input_(ui_, json.toRawUTF8());
  if (result < 0)
    error_ = juce::String::fromUTF8(error_fn_(ui_));
  drain();
  return result > 0;
}
void GpuView::drain() {
  if (!ui_)
    return;
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
