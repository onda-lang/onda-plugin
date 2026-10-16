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

template <typename Class>
static void addOndaFileDropMethods (Class& cls)
{
    cls.template addIvar<WebBrowserComponent*> ("ondaDropOwner");
    cls.addMethod (@selector (draggingEntered:),
                   [] (id self, SEL, id<NSDraggingInfo> sender) -> NSDragOperation
                   {
                       auto* browser = getIvar<WebBrowserComponent*> (self, "ondaDropOwner");
                       auto* target = dynamic_cast<FileDragAndDropTarget*> (browser);
                       return target != nullptr && target->isInterestedInFileDrag (ondaDraggedFiles (sender))
                           ? NSDragOperationCopy : NSDragOperationNone;
                   });
    cls.addMethod (@selector (draggingUpdated:),
                   [] (id self, SEL, id<NSDraggingInfo> sender) -> NSDragOperation
                   {
                       return [self draggingEntered: sender];
                   });
    cls.addMethod (@selector (performDragOperation:),
                   [] (id self, SEL, id<NSDraggingInfo> sender) -> BOOL
                   {
                       auto* browser = getIvar<WebBrowserComponent*> (self, "ondaDropOwner");
                       auto* target = dynamic_cast<FileDragAndDropTarget*> (browser);
                       const auto files = ondaDraggedFiles (sender);
                       if (target == nullptr || !target->isInterestedInFileDrag (files))
                           return NO;
                       target->filesDropped (files, 0, 0);
                       return YES;
                   });
}
