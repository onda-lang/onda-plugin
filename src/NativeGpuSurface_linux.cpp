#include "NativeGpuSurface.h"
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

namespace onda::plugin {
namespace {
class X11Surface final : public NativeGpuSurface {
public:
  explicit X11Surface(std::function<void()> redraw)
      : redraw_(std::move(redraw)), display_(XOpenDisplay(nullptr)) {}
  ~X11Surface() override {
    if (display_) {
      if (registered_)
        juce::LinuxEventLoop::unregisterFdCallback(ConnectionNumber(display_));
      // Closing the connection also destroys the child if the parent still
      // exists. There is no XDestroyWindow request against an already-dead XID.
      XCloseDisplay(display_);
    }
  }
  bool attach(juce::ComponentPeer &peer, juce::String &error) {
    if (!display_) {
      error = "Could not open X11 for GPU UI";
      return false;
    }
    int major{}, minor{};
    if (!XShapeQueryVersion(display_, &major, &minor) || major < 1 ||
        (major == 1 && minor < 1)) {
      error = "GPU UI requires X11 Shape 1.1 for input passthrough";
      return false;
    }
    XSetWindowAttributes attributes{};
    attributes.background_pixmap = None;
    attributes.bit_gravity = NorthWestGravity;
    attributes.event_mask = ExposureMask | StructureNotifyMask;
    window_ = XCreateWindow(
        display_,
        static_cast<::Window>(
            reinterpret_cast<uintptr_t>(peer.getNativeHandle())),
        0, 0, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent,
        CWBackPixmap | CWBitGravity | CWEventMask, &attributes);
    // An empty input shape leaves JUCE's mouse, XDND, and focus handling
    // intact.
    XShapeCombineRectangles(display_, window_, ShapeInput, 0, 0, nullptr, 0,
                            ShapeSet, Unsorted);
    juce::LinuxEventLoop::registerFdCallback(ConnectionNumber(display_),
                                             [this](int) { dispatch(); });
    registered_ = true;
    return true;
  }
  Handles handles() const override {
    return {1, static_cast<uintptr_t>(window_),
            reinterpret_cast<uintptr_t>(display_)};
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
    if (!window_ || bounds == bounds_ || bounds.isEmpty())
      return {bounds.getWidth(), bounds.getHeight()};
    bounds_ = bounds;
    XMoveResizeWindow(display_, window_, bounds.getX(), bounds.getY(),
                      static_cast<unsigned>(bounds.getWidth()),
                      static_cast<unsigned>(bounds.getHeight()));
    // Vulkan must see the new native extent before configuring its swapchain.
    XSync(display_, False);
    dispatch();
    return {bounds.getWidth(), bounds.getHeight()};
  }
  void setVisible(bool visible) override {
    if (!window_ || visible_ == visible)
      return;
    visible_ = visible;
    if (visible)
      XMapWindow(display_, window_);
    else
      XUnmapWindow(display_, window_);
    XFlush(display_);
  }

private:
  void dispatch() {
    bool repaint = false;
    while (XPending(display_)) {
      XEvent event{};
      XNextEvent(display_, &event);
      if (event.type == Expose)
        repaint = true;
      else if (event.type == DestroyNotify)
        window_ = 0;
    }
    if (repaint)
      redraw_();
  }
  std::function<void()> redraw_;
  Display *display_{};
  ::Window window_{};
  juce::Rectangle<int> bounds_;
  bool visible_{};
  bool registered_{};
};
} // namespace

std::unique_ptr<NativeGpuSurface>
NativeGpuSurface::create(juce::ComponentPeer &peer,
                         std::function<void()> redraw, juce::String &error) {
  auto surface = std::make_unique<X11Surface>(std::move(redraw));
  return surface->attach(peer, error) ? std::move(surface) : nullptr;
}
} // namespace onda::plugin
