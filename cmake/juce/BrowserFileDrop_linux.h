// Runs in JUCE's GTK subprocess. Preserve native paths and client coordinates
// over its IPC channel; browser File objects do not expose absolute paths.
static const GtkTargetEntry fileTarget{const_cast<char *>("text/uri-list"), 0, 0};
wk.juce_gtk_drag_dest_set(webviewWidget, static_cast<GtkDestDefaults>(0),
                         &fileTarget, 1, GDK_ACTION_COPY);
juce_g_signal_connect(
    webview, "drag-motion",
    G_CALLBACK(+[](GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                   guint time, gpointer arg) -> gboolean {
      auto &owner = *static_cast<GtkChildProcess *>(arg);
      owner.ondaDragPosition = Point<int>(x, y);
      owner.ondaDragActive = true;
      auto &symbols = *WebKitSymbols::getInstance();
      symbols.juce_gtk_drag_get_data(
          widget, context,
          symbols.juce_gdk_atom_intern_static_string("text/uri-list"), time);
      return TRUE;
    }),
    this);
juce_g_signal_connect(
    webview, "drag-leave",
    G_CALLBACK(+[](GtkWidget *, GdkDragContext *, guint, gpointer arg) {
      auto &owner = *static_cast<GtkChildProcess *>(arg);
      owner.ondaDragActive = false;
      DynamicObject::Ptr message = new DynamicObject;
      message->setProperty("phase", "leave");
      CommandReceiver::sendCommand(owner.outChannel, "ondaFileDrag", var(message.get()));
    }),
    this);
juce_g_signal_connect(
    webview, "drag-drop",
    G_CALLBACK(+[](GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                   guint time, gpointer arg) -> gboolean {
      auto &owner = *static_cast<GtkChildProcess *>(arg);
      owner.ondaDragPosition = Point<int>(x, y);
      owner.ondaDropping = true;
      auto &symbols = *WebKitSymbols::getInstance();
      symbols.juce_gtk_drag_get_data(
          widget, context,
          symbols.juce_gdk_atom_intern_static_string("text/uri-list"), time);
      return TRUE;
    }),
    this);
juce_g_signal_connect(
    webview, "drag-data-received",
    G_CALLBACK(+[](GtkWidget *widget, GdkDragContext *context, gint, gint,
                   GtkSelectionData *data, guint, guint time, gpointer arg) {
      auto &owner = *static_cast<GtkChildProcess *>(arg);
      auto &symbols = *WebKitSymbols::getInstance();
      Array<var> files;
      if (auto **uris = symbols.juce_gtk_selection_data_get_uris(data)) {
        for (auto **uri = uris; *uri != nullptr; ++uri) {
          const URL url{String::fromUTF8(*uri)};
          if (url.isLocalFile())
            files.add(url.getLocalFile().getFullPathName());
        }
        symbols.juce_g_strfreev(uris);
      }
      if (owner.ondaDragActive || owner.ondaDropping) {
        DynamicObject::Ptr message = new DynamicObject;
        message->setProperty("phase", owner.ondaDropping ? "drop" : "over");
        message->setProperty("paths", var(files));
        message->setProperty("x", owner.ondaDragPosition.x);
        message->setProperty("y", owner.ondaDragPosition.y);
        CommandReceiver::sendCommand(owner.outChannel, "ondaFileDrag", var(message.get()));
      }
      if (owner.ondaDropping) {
        owner.ondaDropping = owner.ondaDragActive = false;
        symbols.juce_gtk_drag_finish(context, !files.isEmpty(), FALSE, time);
      } else if (owner.ondaDragActive) {
        symbols.juce_gdk_drag_status(
            context, files.isEmpty() ? static_cast<GdkDragAction>(0) : GDK_ACTION_COPY, time);
      }
      symbols.juce_g_signal_stop_emission_by_name(widget, "drag-data-received");
    }),
    this);
