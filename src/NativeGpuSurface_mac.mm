#include "NativeGpuSurface.h"
#import <Cocoa/Cocoa.h>

@interface ONDA_GPU_NS_VIEW_CLASS : NSView {
@public
  std::function<void()> redraw;
}
@end
@implementation ONDA_GPU_NS_VIEW_CLASS
- (NSView *)hitTest:(NSPoint)point {
  (void)point;
  return nil;
}
- (BOOL)isOpaque {
  return YES;
}
- (BOOL)isFlipped {
  return YES;
}
- (void)drawRect:(NSRect)rect {
  (void)rect;
  if (redraw)
    redraw();
}
- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  if (redraw)
    redraw();
}
@end

namespace onda::plugin {
namespace {
class MacSurface final : public NativeGpuSurface {
public:
  MacSurface(juce::ComponentPeer &peer, std::function<void()> redraw) {
    view_ =
        [[ONDA_GPU_NS_VIEW_CLASS alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
    view_->redraw = std::move(redraw);
    [view_ setHidden:YES];
    [view_ setWantsLayer:YES];
    [view_
        setLayerContentsRedrawPolicy:NSViewLayerContentsRedrawDuringViewResize];
    [(NSView *)peer.getNativeHandle() addSubview:view_];
  }
  ~MacSurface() override {
    view_->redraw = {};
    [view_ removeFromSuperview];
    [view_ release];
  }
  Handles handles() const override {
    return {3, reinterpret_cast<uintptr_t>(view_)};
  }
  juce::Point<int> setBounds(juce::Rectangle<int> bounds,
                             float scale) override {
    auto *parent = [view_ superview];
    const auto y = [parent isFlipped]
                       ? bounds.getY()
                       : [parent bounds].size.height - bounds.getBottom();
    const auto frame =
        NSMakeRect(bounds.getX(), y, bounds.getWidth(), bounds.getHeight());
    if (!NSEqualRects(frame, [view_ frame]))
      [view_ setFrame:frame];
    return (juce::Point<float>{static_cast<float>(bounds.getWidth()),
                               static_cast<float>(bounds.getHeight())} *
            scale)
        .roundToInt();
  }
  void setVisible(bool visible) override { [view_ setHidden:!visible]; }

private:
  ONDA_GPU_NS_VIEW_CLASS *view_{};
};
} // namespace

std::unique_ptr<NativeGpuSurface>
NativeGpuSurface::create(juce::ComponentPeer &peer,
                         std::function<void()> redraw, juce::String &) {
  return std::make_unique<MacSurface>(peer, std::move(redraw));
}
} // namespace onda::plugin
