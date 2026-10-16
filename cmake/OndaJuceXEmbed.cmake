# The pinned JUCE XEmbed implementation captures a raw Pimpl in a deferred
# resize callback. Closing the editor can delete it before the callback runs.
# Patch build copies so both shipped plugins and native tests use the fix.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

set(onda_gui_extra_directory "${ONDA_JUCE_SOURCE_DIR}/modules/juce_gui_extra")
set(onda_xembed_original "${onda_gui_extra_directory}/native/juce_XEmbedComponent_linux.cpp")
set(onda_xembed_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/juce-xembed")
file(READ "${onda_xembed_original}" onda_xembed_source)
set(onda_xembed_callback [=[MessageManager::callAsync ([this] { componentMovedOrResized (owner, true, true); });]=])
set(onda_xembed_class_end [=[        return SharedKeyWindow::getCurrentFocusWindow (p);
    }
};]=])
foreach(anchor IN ITEMS "${onda_xembed_callback}" "${onda_xembed_class_end}")
    string(FIND "${onda_xembed_source}" "${anchor}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Could not locate the pinned JUCE XEmbed callback lifetime anchors")
    endif()
endforeach()
string(REPLACE "${onda_xembed_callback}" [=[MessageManager::callAsync ([safeThis = WeakReference<Pimpl> (this)]
                        {
                            if (safeThis != nullptr)
                                safeThis->componentMovedOrResized (safeThis->owner, true, true);
                        });]=] onda_xembed_source "${onda_xembed_source}")
string(REPLACE "${onda_xembed_class_end}" [=[        return SharedKeyWindow::getCurrentFocusWindow (p);
    }

    JUCE_DECLARE_WEAK_REFERENCEABLE (Pimpl)
};]=] onda_xembed_source "${onda_xembed_source}")
set(onda_xembed_patched "${onda_xembed_directory}/juce_XEmbedComponent_linux.cpp")
file(CONFIGURE OUTPUT "${onda_xembed_patched}" CONTENT "${onda_xembed_source}" @ONLY)

# OndaJuceBrowser.cmake assembles the GUI-extra compilation unit with both fixes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${onda_xembed_original}")
