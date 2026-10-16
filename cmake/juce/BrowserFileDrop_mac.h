// WKWebView consumes native drags before JUCE's parent component sees them.
// Forward file paths to the browser's FileDragAndDropTarget, as standalone Wry does.
static StringArray ondaDraggedFiles (id<NSDraggingInfo> sender)
{
    StringArray files;
    auto* urls = [[sender draggingPasteboard]
        readObjectsForClasses: @[ [NSURL class] ]
        options: @{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    for (NSURL* url in urls)
        files.add (nsStringToJuce ([url path]));
    return files;
}

static Point<int> ondaDragPosition (NSView* view, id<NSDraggingInfo> sender)
{
    const auto point = [view convertPoint: [sender draggingLocation] fromView: nil];
    const auto y = [view isFlipped] ? point.y : [view bounds].size.height - point.y;
    return { roundToInt (point.x), roundToInt (y) };
}

template <typename Class>
static void addOndaFileDropMethods (Class& cls)
{
    cls.template addIvar<WebBrowserComponent*> ("ondaDropOwner");
    cls.addMethod (@selector (draggingEntered:),
                   [] (id self, SEL, id<NSDraggingInfo> sender) -> NSDragOperation
                   {
                       auto* browser = getIvar<WebBrowserComponent*> (self, "ondaDropOwner");
                       auto* target = dynamic_cast<FileDragAndDropTarget*> (browser);
                       const auto files = ondaDraggedFiles (sender);
                       if (target == nullptr || !target->isInterestedInFileDrag (files))
                           return NSDragOperationNone;
                       const auto point = ondaDragPosition ((NSView*) self, sender);
                       target->fileDragMove (files, point.x, point.y);
                       return NSDragOperationCopy;
                   });
    cls.addMethod (@selector (draggingUpdated:),
                   [] (id self, SEL, id<NSDraggingInfo> sender) -> NSDragOperation
                   {
                       return [self draggingEntered: sender];
                   });
    cls.addMethod (@selector (draggingExited:),
                   [] (id self, SEL, id<NSDraggingInfo> sender)
                   {
                       auto* browser = getIvar<WebBrowserComponent*> (self, "ondaDropOwner");
                       if (auto* target = dynamic_cast<FileDragAndDropTarget*> (browser))
                           target->fileDragExit (ondaDraggedFiles (sender));
                   });
    cls.addMethod (@selector (performDragOperation:),
                   [] (id self, SEL, id<NSDraggingInfo> sender) -> BOOL
                   {
                       auto* browser = getIvar<WebBrowserComponent*> (self, "ondaDropOwner");
                       auto* target = dynamic_cast<FileDragAndDropTarget*> (browser);
                       const auto files = ondaDraggedFiles (sender);
                       if (target == nullptr || !target->isInterestedInFileDrag (files))
                           return NO;
                       const auto point = ondaDragPosition ((NSView*) self, sender);
                       target->filesDropped (files, point.x, point.y);
                       return YES;
                   });
}
