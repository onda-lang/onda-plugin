# Build copies of pinned JUCE fixes without modifying a shared checkout.
function(onda_gui_basics_replace source_variable anchor replacement)
    string(FIND "${${source_variable}}" "${anchor}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Could not locate pinned JUCE GUI-basics anchor: ${anchor}")
    endif()
    string(REPLACE "${anchor}" "${replacement}" source "${${source_variable}}")
    set(${source_variable} "${source}" PARENT_SCOPE)
endfunction()

set(onda_gui_basics_directory "${ONDA_JUCE_SOURCE_DIR}/modules/juce_gui_basics")
set(onda_gui_basics_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/juce-gui-basics")
set(onda_component_original "${onda_gui_basics_directory}/components/juce_Component.cpp")
file(READ "${onda_component_original}" onda_component_source)
set(onda_peer_removal [=[        flags.hasHeavyweightPeerFlag = false;
        delete peer;

        Desktop::getInstance().removeDesktopComponent (this);]=])
# Match addToDesktop's peer-replacement ordering: notify children while the old
# native peer is still alive so borrowed GPU surfaces can be released safely.
onda_gui_basics_replace(onda_component_source "${onda_peer_removal}" [=[        std::unique_ptr<ComponentPeer> oldPeerToDelete (peer);
        flags.hasHeavyweightPeerFlag = false;
        Desktop::getInstance().removeDesktopComponent (this);
        internalHierarchyChanged();]=])
set(onda_component_patched "${onda_gui_basics_generated}/juce_Component.cpp")
file(CONFIGURE OUTPUT "${onda_component_patched}" CONTENT "${onda_component_source}" @ONLY)

set(onda_gui_basics_cpp "${onda_gui_basics_directory}/juce_gui_basics.cpp")
file(READ "${onda_gui_basics_cpp}" onda_gui_basics_source)
onda_gui_basics_replace(onda_gui_basics_source "#include \"components/juce_Component.cpp\""
    "#include \"${onda_component_patched}\"")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include("${CMAKE_CURRENT_LIST_DIR}/OndaJuceFileDrop.cmake")
    onda_gui_basics_replace(onda_gui_basics_source "#include \"native/juce_DragAndDrop_linux.cpp\""
        "#include \"${onda_file_drop_native_patched}\"")
endif()

# JUCE's macOS .mm wrapper includes this same body; retain its source language.
if(APPLE)
    set(onda_gui_basics_extension mm)
else()
    set(onda_gui_basics_extension cpp)
endif()
set(onda_gui_basics_original "${onda_gui_basics_directory}/juce_gui_basics.${onda_gui_basics_extension}")
set(onda_gui_basics_patched "${onda_gui_basics_generated}/juce_gui_basics.${onda_gui_basics_extension}")
file(CONFIGURE OUTPUT "${onda_gui_basics_patched}" CONTENT "${onda_gui_basics_source}" @ONLY)
get_target_property(onda_gui_basics_sources juce_gui_basics INTERFACE_SOURCES)
list(FIND onda_gui_basics_sources "${onda_gui_basics_original}" onda_gui_basics_position)
if(onda_gui_basics_position EQUAL -1)
    message(FATAL_ERROR "Could not find JUCE's GUI-basics compilation unit")
endif()
list(REMOVE_ITEM onda_gui_basics_sources "${onda_gui_basics_original}")
list(APPEND onda_gui_basics_sources "${onda_gui_basics_patched}")
set_property(TARGET juce_gui_basics PROPERTY INTERFACE_SOURCES "${onda_gui_basics_sources}")
set_property(SOURCE "${onda_gui_basics_patched}" APPEND PROPERTY
    INCLUDE_DIRECTORIES "${onda_gui_basics_directory}")
if(NOT MSVC)
    set_property(SOURCE "${onda_gui_basics_patched}" APPEND PROPERTY COMPILE_OPTIONS -w)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${onda_component_original}" "${onda_gui_basics_cpp}" "${onda_gui_basics_original}")
