# Correct native file offers, coordinates in moved host windows, and XDND
# timestamps in build copies of the pinned JUCE backend.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

set(onda_file_drop_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/juce-file-drop")
set(onda_file_drop_module_directory "${ONDA_JUCE_SOURCE_DIR}/modules/juce_gui_basics")
set(onda_file_drop_native_original "${onda_file_drop_module_directory}/native/juce_DragAndDrop_linux.cpp")
file(READ "${onda_file_drop_native_original}" onda_file_drop_source)
# Nautilus offers text before URIs; choose files while preserving text fallback.
set(onda_file_drop_preference [=[        {
            XWindowSystemUtilities::ScopedXLock xLock;
            const auto fileType = XWindowSystemUtilities::Atoms::getCreating (getDisplay(), "text/uri-list");
            if (srcMimeTypeAtomList.contains (fileType))
                dragAndDropCurrentMimeType = fileType;
        }

]=])
set(onda_file_drop_loop "        for (int i = 0; i < srcMimeTypeAtomList.size() && dragAndDropCurrentMimeType == 0; ++i)")
onda_gui_basics_replace(onda_file_drop_source "${onda_file_drop_loop}" "${onda_file_drop_preference}${onda_file_drop_loop}")

# JUCE caches the host parent's screen position. Translate directly into the
# native peer so moving the host window cannot move a drop outside the editor.
onda_gui_basics_replace(onda_file_drop_source [=[        const auto& displays = Desktop::getInstance().getDisplays();
        const auto logicalPos = displays.physicalToLogical (Point ((int) clientMsg.data.l[2] >> 16,
                                                                   (int) clientMsg.data.l[2] & 0xffff).toFloat());
        const auto dropPos = detail::ScalingHelpers::screenPosToLocalPos (peer->getComponent(), logicalPos).roundToInt();]=]
    [=[        Point<int> localPhysicalPos;
        {
            XWindowSystemUtilities::ScopedXLock xLock;
            auto* display = getDisplay();
            auto* x11 = X11Symbols::getInstance();
            ::Window child;
            if (! x11->xTranslateCoordinates (display, x11->xDefaultRootWindow (display),
                    (::Window) peer->getNativeHandle(), (int) clientMsg.data.l[2] >> 16,
                    (int) clientMsg.data.l[2] & 0xffff, &localPhysicalPos.x, &localPhysicalPos.y, &child))
                return;
        }
        const auto dropPos = detail::ScalingHelpers::unscaledScreenPosToScaled (peer->getComponent(),
            localPhysicalPos.toFloat() / peer->getPlatformScaleFactor()).roundToInt();]=])

# Enter has MIME atoms, not coordinates or timestamps. Request data on Position
# or Drop, using their respective timestamp fields from the XDND specification.
onda_gui_basics_replace(onda_file_drop_source "        handleDragAndDropPosition (clientMsg, peer);" "")
onda_gui_basics_replace(onda_file_drop_source "    void updateDraggedFileList (const XClientMessageEvent& clientMsg, ::Window requestor)"
    "    void updateDraggedFileList (::Time time, ::Window requestor)")
onda_gui_basics_replace(onda_file_drop_source "requestor, (::Time) clientMsg.data.l[2]);" "requestor, time);")
onda_gui_basics_replace(onda_file_drop_source "updateDraggedFileList (clientMsg, (::Window) peer->getNativeHandle());"
    "updateDraggedFileList ((::Time) clientMsg.data.l[3], (::Window) peer->getNativeHandle());")
onda_gui_basics_replace(onda_file_drop_source [=[finishAfterDropDataReceived = true;
            updateDraggedFileList ((::Time) clientMsg.data.l[3],]=]
    [=[finishAfterDropDataReceived = true;
            updateDraggedFileList ((::Time) clientMsg.data.l[2],]=])
set(onda_file_drop_native_patched "${onda_file_drop_directory}/juce_DragAndDrop_linux.cpp")
file(CONFIGURE OUTPUT "${onda_file_drop_native_patched}" CONTENT "${onda_file_drop_source}" @ONLY)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${onda_file_drop_native_original}")
