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

# Replace only the affected include in the module's compilation unit. All
# other JUCE sources and relative includes still come from the pinned checkout.
set(onda_gui_extra_original "${onda_gui_extra_directory}/juce_gui_extra.cpp")
set(onda_gui_extra_patched "${onda_xembed_directory}/juce_gui_extra.cpp")
file(READ "${onda_gui_extra_original}" onda_gui_extra_source)
set(onda_xembed_include "#include \"native/juce_XEmbedComponent_linux.cpp\"")
string(FIND "${onda_gui_extra_source}" "${onda_xembed_include}" onda_xembed_include_position)
if(onda_xembed_include_position EQUAL -1)
    message(FATAL_ERROR "Could not find JUCE's XEmbed implementation include")
endif()
string(REPLACE "${onda_xembed_include}"
    "#include \"${onda_xembed_patched}\""
    onda_gui_extra_source "${onda_gui_extra_source}")
file(CONFIGURE OUTPUT "${onda_gui_extra_patched}" CONTENT "${onda_gui_extra_source}" @ONLY)
get_target_property(onda_gui_extra_sources juce_gui_extra INTERFACE_SOURCES)
list(FIND onda_gui_extra_sources "${onda_gui_extra_original}" onda_gui_extra_index)
if(onda_gui_extra_index EQUAL -1)
    message(FATAL_ERROR "Could not find JUCE's GUI extra compilation unit")
endif()
list(REMOVE_ITEM onda_gui_extra_sources "${onda_gui_extra_original}")
list(APPEND onda_gui_extra_sources "${onda_gui_extra_patched}")
set_property(TARGET juce_gui_extra PROPERTY INTERFACE_SOURCES "${onda_gui_extra_sources}")
set_property(SOURCE "${onda_gui_extra_patched}" APPEND PROPERTY
    INCLUDE_DIRECTORIES "${onda_gui_extra_directory}")
set_property(SOURCE "${onda_gui_extra_patched}" APPEND PROPERTY COMPILE_OPTIONS -w)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${onda_xembed_original}" "${onda_gui_extra_original}")
