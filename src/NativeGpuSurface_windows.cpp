#include "NativeGpuSurface.h"
#include <windows.h>

namespace onda::plugin {
namespace {
class DpiScope {
public:
  explicit DpiScope(HWND window)
      : previous_(SetThreadDpiAwarenessContext(
            GetWindowDpiAwarenessContext(window))) {}
  ~DpiScope() {
    if (previous_)
      SetThreadDpiAwarenessContext(previous_);
  }

private:
  DPI_AWARENESS_CONTEXT previous_{};
};

class WindowsSurface final : public NativeGpuSurface {
public:
  explicit WindowsSurface(std::function<void()> redraw)
      : redraw_(std::move(redraw)) {}
  ~WindowsSurface() override {
    if (window_)
      DestroyWindow(window_);
  }
  bool attach(juce::ComponentPeer &peer, juce::String &error) {
    const DpiScope dpi(static_cast<HWND>(peer.getNativeHandle()));
    static WindowClass windowClass;
    if (windowClass.atom)
      window_ =
          CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, className, L"",
                          WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 1,
                          1, static_cast<HWND>(peer.getNativeHandle()), nullptr,
                          windowClass.instance, this);
    if (!window_)
      error =
          "Could not create GPU child window: " + juce::String(GetLastError());
    return window_ != nullptr;
  }
  Handles handles() const override {
    return {2, reinterpret_cast<uintptr_t>(window_)};
  }
  juce::Point<int> setBounds(juce::Rectangle<int> logical,
                             float scale) override {
    const auto position =
        (logical.getPosition().toFloat() * scale).roundToInt();
    const auto size =
        (juce::Point<float>{static_cast<float>(logical.getWidth()),
                            static_cast<float>(logical.getHeight())} *
         scale)
            .roundToInt();
    const juce::Rectangle<int> bounds{position.x, position.y, size.x, size.y};
    if (window_ && bounds != bounds_) {
      const DpiScope dpi(window_);
      bounds_ = bounds;
      SetWindowPos(window_, nullptr, bounds.getX(), bounds.getY(),
                   bounds.getWidth(), bounds.getHeight(),
                   SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    return {bounds.getWidth(), bounds.getHeight()};
  }
  void setVisible(bool visible) override {
    if (window_ && visible_ != visible) {
      visible_ = visible;
      ShowWindow(window_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
  }

private:
  static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w,
                                     LPARAM l) {
    auto *self = reinterpret_cast<WindowsSurface *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<WindowsSurface *>(
          reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_NCHITTEST)
      return HTTRANSPARENT;
    if (message == WM_ERASEBKGND)
      return 1;
    if (message == WM_PAINT) {
      PAINTSTRUCT paint{};
      BeginPaint(window, &paint);
      EndPaint(window, &paint);
      if (self)
        self->redraw_();
      return 0;
    }
    if (self && message == WM_NCDESTROY) {
      self->window_ = nullptr;
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(window, message, w, l);
  }
  static constexpr auto className = L"OndaGpuSurface";
  struct WindowClass {
    WindowClass() {
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&windowProc), &instance);
      WNDCLASSEXW descriptor{};
      descriptor.cbSize = sizeof(descriptor);
      descriptor.lpfnWndProc = windowProc;
      descriptor.hInstance = instance;
      descriptor.lpszClassName = className;
      atom = RegisterClassExW(&descriptor);
    }
    ~WindowClass() {
      if (atom)
        UnregisterClassW(className, instance);
    }
    HINSTANCE instance{};
    ATOM atom{};
  };
  std::function<void()> redraw_;
  HWND window_{};
  juce::Rectangle<int> bounds_;
  bool visible_{};
};
} // namespace

std::unique_ptr<NativeGpuSurface>
NativeGpuSurface::create(juce::ComponentPeer &peer,
                         std::function<void()> redraw, juce::String &error) {
  auto surface = std::make_unique<WindowsSurface>(std::move(redraw));
  return surface->attach(peer, error) ? std::move(surface) : nullptr;
}
} // namespace onda::plugin
