// Runs in JUCE's GTK subprocess. Request native URI data on drop and carry the
// original paths over its existing IPC channel; browser File objects lose paths.
static const GtkTargetEntry fileTarget{const_cast<char *>("text/uri-list"), 0,
                                       0};
wk.juce_gtk_drag_dest_set(webviewWidget, static_cast<GtkDestDefaults>(0),
                          &fileTarget, 1, GDK_ACTION_COPY);
juce_g_signal_connect(webview, "drag-motion",
                      G_CALLBACK(+[](GtkWidget *, GdkDragContext *context, gint,
                                     gint, guint time, gpointer) -> gboolean {
                        WebKitSymbols::getInstance()->juce_gdk_drag_status(
                            context, GDK_ACTION_COPY, time);
                        return TRUE;
                      }),
                      this);
juce_g_signal_connect(
    webview, "drag-drop",
    G_CALLBACK(+[](GtkWidget *widget, GdkDragContext *context, gint, gint,
                   guint time, gpointer) -> gboolean {
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
      if (!files.isEmpty())
        CommandReceiver::sendCommand(
            static_cast<GtkChildProcess *>(arg)->outChannel, "ondaFilesDropped",
            var(files));
      symbols.juce_gtk_drag_finish(context, !files.isEmpty(), FALSE, time);
      symbols.juce_g_signal_stop_emission_by_name(widget, "drag-data-received");
    }),
    this);
