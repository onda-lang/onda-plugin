#include "NativeFileDrop.h"

#import <Cocoa/Cocoa.h>

@interface OndaFileDropTestInfo : NSObject
@property(nonatomic, retain) NSPasteboard *pasteboard;
@property(nonatomic) NSPoint location;
@end

@implementation OndaFileDropTestInfo
- (NSPasteboard *)draggingPasteboard {
  return self.pasteboard;
}
- (NSPoint)draggingLocation {
  return self.location;
}
- (void)dealloc {
  [_pasteboard release];
  [super dealloc];
}
@end

namespace {
NSView *findNativeView(juce::Component &component) {
  if (auto *native = dynamic_cast<juce::NSViewComponent *>(&component))
    return static_cast<NSView *>(native->getView());
  for (auto *child : component.getChildren())
    if (auto *view = findNativeView(*child))
      return view;
  return nil;
}
} // namespace

bool dropNativeFile(juce::WebBrowserComponent &browser, const juce::File &file,
                    juce::Point<int> position) {
  @autoreleasepool {
    auto *view = findNativeView(browser);
    if (!view)
      return false;
    auto *pasteboard = [NSPasteboard pasteboardWithUniqueName];
    NSString *path =
        [NSString stringWithUTF8String:file.getFullPathName().toRawUTF8()];
    [pasteboard writeObjects:@[ [NSURL fileURLWithPath:path] ]];
    auto *info = [[OndaFileDropTestInfo alloc] init];
    info.pasteboard = pasteboard;
    const auto y =
        [view isFlipped] ? position.y : [view bounds].size.height - position.y;
    info.location = [view convertPoint:NSMakePoint(position.x, y) toView:nil];
    id<NSDraggingInfo> sender = (id<NSDraggingInfo>)info;
    const auto accepted =
        [view draggingEntered:sender] == NSDragOperationCopy &&
        [view draggingUpdated:sender] == NSDragOperationCopy;
    const auto dropped = accepted && [view performDragOperation:sender];
    [info release];
    [pasteboard releaseGlobally];
    return dropped;
  }
}
