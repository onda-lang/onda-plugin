# JUCE's native webviews intercept file drags before FileDragAndDropTarget.
# Patch build copies to forward native paths, including the GTK subprocess.
if(NOT APPLE AND NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

function(onda_browser_replace anchor replacement)
    string(FIND "${onda_browser_source}" "${anchor}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Could not locate pinned JUCE browser anchor: ${anchor}")
    endif()
    string(REPLACE "${anchor}" "${replacement}" onda_browser_source "${onda_browser_source}")
    set(onda_browser_source "${onda_browser_source}" PARENT_SCOPE)
endfunction()

set(onda_browser_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/juce-browser")
set(onda_browser_module_directory "${ONDA_JUCE_SOURCE_DIR}/modules/juce_gui_extra")
if(APPLE)
    set(onda_browser_native_name "juce_WebBrowserComponent_mac.mm")
elseif(WIN32)
    set(onda_browser_native_name "juce_WebBrowserComponent_windows.cpp")
else()
    set(onda_browser_native_name "juce_WebBrowserComponent_linux.cpp")
endif()
set(onda_browser_native_original "${onda_browser_module_directory}/native/${onda_browser_native_name}")
file(READ "${onda_browser_native_original}" onda_browser_source)

if(APPLE)
    onda_browser_replace("template <class WebViewClass>"
        "#include \"${CMAKE_CURRENT_LIST_DIR}/juce/BrowserFileDrop_mac.h\"\ntemplate <class WebViewClass>")
    onda_browser_replace("this->template addIvar<LastFocusChange*> (lastFocusChangeMemberName);"
        "this->template addIvar<LastFocusChange*> (lastFocusChangeMemberName);\n        addOndaFileDropMethods (*this);")
    onda_browser_replace("setLastFocusChangeHandle (webView.get(), &lastFocusChange);\n       #else"
        "setLastFocusChangeHandle (webView.get(), &lastFocusChange);\n        object_setInstanceVariable (webView.get(), \"ondaDropOwner\", &owner.owner);\n        [webView.get() registerForDraggedTypes: @[ NSPasteboardTypeFileURL ]];\n       #else")
elseif(WIN32)
    onda_browser_replace("class WebBrowserComponent::Impl::Platform::WebView2 final"
        "#include \"${CMAKE_CURRENT_LIST_DIR}/juce/BrowserFileDrop_windows.h\"\nclass WebBrowserComponent::Impl::Platform::WebView2 final")
    onda_browser_replace("owner.impl->handleNativeEvent (JSON::fromString (*str));"
        [=[{
                                                             if (*str == "ondaFilesDropped")
                                                                 ondaWebViewFilesDropped (owner, args);
                                                             else
                                                                 owner.impl->handleNativeEvent (JSON::fromString (*str));
                                                         }]=])
else()
    set(onda_browser_symbols [=[
    JUCE_GENERATE_FUNCTION_WITH_DEFAULT (gtk_drag_dest_set, juce_gtk_drag_dest_set,
        (GtkWidget*, GtkDestDefaults, const GtkTargetEntry*, gint, GdkDragAction), void)
    JUCE_GENERATE_FUNCTION_WITH_DEFAULT (gtk_drag_get_data, juce_gtk_drag_get_data,
        (GtkWidget*, GdkDragContext*, GdkAtom, guint32), void)
    JUCE_GENERATE_FUNCTION_WITH_DEFAULT (gdk_atom_intern_static_string, juce_gdk_atom_intern_static_string,
        (const gchar*), GdkAtom)
    JUCE_GENERATE_FUNCTION_WITH_DEFAULT (gdk_drag_status, juce_gdk_drag_status,
        (GdkDragContext*, GdkDragAction, guint32), void)
    JUCE_GENERATE_FUNCTION_WITH_DEFAULT (gtk_selection_data_get_uris, juce_gtk_selection_data_get_uris,
        (const GtkSelectionData*), gchar**)
    JUCE_GENERATE_FUNCTION_WITH_DEFAULT (g_strfreev, juce_g_strfreev, (gchar**), void)
    JUCE_GENERATE_FUNCTION_WITH_DEFAULT (gtk_drag_finish, juce_gtk_drag_finish,
        (GdkDragContext*, gboolean, gboolean, guint32), void)
    JUCE_GENERATE_FUNCTION_WITH_DEFAULT (g_signal_stop_emission_by_name, juce_g_signal_stop_emission_by_name,
        (gpointer, const gchar*), void)
]=])
    onda_browser_replace("bool isWebKitAvailable() const noexcept  { return webKitIsAvailable; }"
        "bool isWebKitAvailable() const noexcept  { return webKitIsAvailable; }\n${onda_browser_symbols}")
    set(onda_browser_bindings "")
    foreach(symbol IN ITEMS gtk_drag_dest_set gtk_drag_get_data gdk_atom_intern_static_string
            gdk_drag_status gtk_selection_data_get_uris g_strfreev gtk_drag_finish g_signal_stop_emission_by_name)
        string(APPEND onda_browser_bindings "makeSymbolBinding (juce_${symbol}, \"${symbol}\"),\n                            ")
    endforeach()
    onda_browser_replace("makeSymbolBinding (juce_gtk_main,"
        "${onda_browser_bindings}makeSymbolBinding (juce_gtk_main,")
    onda_browser_replace("webview = (WebKitWebView*) webviewWidget;"
        "webview = (WebKitWebView*) webviewWidget;\n#include \"${CMAKE_CURRENT_LIST_DIR}/juce/BrowserFileDrop_linux.h\"")
    # CommandReceiver sends UTF-8 JSON; String(char*, size) interprets Latin-1.
    # Decode its bytes explicitly so source paths survive the subprocess hop.
    onda_browser_replace("String (buffer.getData(), lengthsBuffer.getJsonLength())"
        "String::fromUTF8 (buffer.getData(), static_cast<int> (lengthsBuffer.getJsonLength()))")
    onda_browser_replace("if      (cmd == \"pageAboutToLoad\")"
        [=[if (cmd == "ondaFilesDropped")
        {
            if (auto* target = dynamic_cast<FileDragAndDropTarget*> (&browser))
            {
                StringArray files;
                if (const auto* paths = params.getArray())
                    for (const auto& path : *paths)
                        files.add (path.toString());
                if (target->isInterestedInFileDrag (files))
                    target->filesDropped (files, 0, 0);
            }
        }
        else if (cmd == "pageAboutToLoad")]=])
endif()

set(onda_browser_native_patched "${onda_browser_directory}/${onda_browser_native_name}")
file(CONFIGURE OUTPUT "${onda_browser_native_patched}" CONTENT "${onda_browser_source}" @ONLY)

# Assemble the GUI-extra compilation unit from the patched native implementations.
get_target_property(onda_browser_module_sources juce_gui_extra INTERFACE_SOURCES)
set(onda_browser_module_original "${onda_browser_module_directory}/juce_gui_extra.cpp")
file(READ "${onda_browser_module_original}" onda_browser_source)
onda_browser_replace("#include \"native/${onda_browser_native_name}\""
    "#include \"${onda_browser_native_patched}\"")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    onda_browser_replace("#include \"native/juce_XEmbedComponent_linux.cpp\""
        "#include \"${onda_xembed_patched}\"")
endif()
if(APPLE)
    set(onda_browser_module_target "${onda_browser_module_directory}/juce_gui_extra.mm")
    set(onda_browser_module_patched "${onda_browser_directory}/juce_gui_extra.mm")
else()
    set(onda_browser_module_target "${onda_browser_module_original}")
    set(onda_browser_module_patched "${onda_browser_directory}/juce_gui_extra.cpp")
endif()
list(FIND onda_browser_module_sources "${onda_browser_module_target}" onda_browser_module_index)
if(onda_browser_module_index EQUAL -1)
    message(FATAL_ERROR "Could not find JUCE's GUI-extra compilation unit")
endif()
file(CONFIGURE OUTPUT "${onda_browser_module_patched}" CONTENT "${onda_browser_source}" @ONLY)
list(REMOVE_ITEM onda_browser_module_sources "${onda_browser_module_target}")
list(APPEND onda_browser_module_sources "${onda_browser_module_patched}")
set_property(TARGET juce_gui_extra PROPERTY INTERFACE_SOURCES "${onda_browser_module_sources}")
set_property(SOURCE "${onda_browser_module_patched}" APPEND PROPERTY
    INCLUDE_DIRECTORIES "${onda_browser_module_directory}")
if(NOT MSVC)
    set_property(SOURCE "${onda_browser_module_patched}" APPEND PROPERTY COMPILE_OPTIONS -w)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${onda_browser_native_original}" "${onda_browser_module_original}")
