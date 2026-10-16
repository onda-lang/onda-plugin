// WebView2's supported File-object bridge retains the original absolute paths.
// https://learn.microsoft.com/microsoft-edge/webview2/reference/win32/icorewebview2webmessagereceivedeventargs2
static void ondaWebViewFilesDropped (WebBrowserComponent& browser,
                                    ICoreWebView2WebMessageReceivedEventArgs* args,
                                    const var& message)
{
    auto* target = dynamic_cast<FileDragAndDropTarget*> (&browser);
    if (target == nullptr)
        return;

    ComSmartPtr<ICoreWebView2WebMessageReceivedEventArgs2> extended;
    args->QueryInterface (extended.resetAndGetPointerAddress());
    if (extended == nullptr)
        return;

    ComSmartPtr<ICoreWebView2ObjectCollectionView> objects;
    if (FAILED (extended->get_AdditionalObjects (objects.resetAndGetPointerAddress())) || objects == nullptr)
        return;

    UINT32 count{};
    if (FAILED (objects->get_Count (&count)))
        return;

    StringArray files;
    for (UINT32 index = 0; index < count; ++index)
    {
        ComSmartPtr<IUnknown> object;
        if (FAILED (objects->GetValueAtIndex (index, object.resetAndGetPointerAddress())) || object == nullptr)
            continue;
        ComSmartPtr<ICoreWebView2File> file;
        object.QueryInterface (file);
        if (file == nullptr)
            continue;

        LPWSTR path{};
        const auto result = file->get_Path (&path);
        const ScopeGuard freePath { [&] { CoTaskMemFree (path); } };
        if (SUCCEEDED (result) && path != nullptr)
            files.add (String (path));
    }
    if (target->isInterestedInFileDrag (files))
        target->filesDropped (files, static_cast<int> (message["x"]),
                              static_cast<int> (message["y"]));
}
