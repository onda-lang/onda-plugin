cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED ONDA_PLUGIN_VERSION OR ONDA_PLUGIN_VERSION STREQUAL "")
    if(DEFINED ENV{ONDA_PLUGIN_VERSION}
            AND NOT "$ENV{ONDA_PLUGIN_VERSION}" STREQUAL "")
        set(ONDA_PLUGIN_VERSION "$ENV{ONDA_PLUGIN_VERSION}")
    else()
        set(ONDA_PLUGIN_VERSION_FILE
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin-version")
        if(NOT EXISTS "${ONDA_PLUGIN_VERSION_FILE}")
            message(FATAL_ERROR
                "Plugin version file not found: ${ONDA_PLUGIN_VERSION_FILE}")
        endif()
        file(STRINGS "${ONDA_PLUGIN_VERSION_FILE}" ONDA_PLUGIN_VERSION
            LIMIT_COUNT 1)
    endif()
endif()
string(STRIP "${ONDA_PLUGIN_VERSION}" ONDA_PLUGIN_VERSION)
if(NOT ONDA_PLUGIN_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Invalid plugin version '${ONDA_PLUGIN_VERSION}'")
endif()

if(NOT DEFINED ONDA_VERSION OR ONDA_VERSION STREQUAL "")
    if(DEFINED ENV{ONDA_VERSION} AND NOT "$ENV{ONDA_VERSION}" STREQUAL "")
        set(ONDA_VERSION "$ENV{ONDA_VERSION}")
    else()
        set(ONDA_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/onda-version")
        if(NOT EXISTS "${ONDA_VERSION_FILE}")
            message(FATAL_ERROR
                "Onda version file not found: ${ONDA_VERSION_FILE}")
        endif()
        file(STRINGS "${ONDA_VERSION_FILE}" ONDA_VERSION LIMIT_COUNT 1)
    endif()
endif()
string(STRIP "${ONDA_VERSION}" ONDA_VERSION)
if(NOT ONDA_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Invalid Onda version '${ONDA_VERSION}'")
endif()

set(JUCE_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/juce-version")
if(NOT EXISTS "${JUCE_VERSION_FILE}")
    message(FATAL_ERROR "JUCE version file not found: ${JUCE_VERSION_FILE}")
endif()
file(STRINGS "${JUCE_VERSION_FILE}" JUCE_REVISION LIMIT_COUNT 1)
string(STRIP "${JUCE_REVISION}" JUCE_REVISION)
string(LENGTH "${JUCE_REVISION}" JUCE_REVISION_LENGTH)
if(NOT JUCE_REVISION MATCHES "^[0-9a-f]+$" OR
        NOT JUCE_REVISION_LENGTH EQUAL 40)
    message(FATAL_ERROR "Invalid JUCE revision '${JUCE_REVISION}'")
endif()

# Floating-point std::to_chars cannot back-deploy to the SDK's macOS 11 baseline.
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()
project(onda-plugin VERSION "${ONDA_PLUGIN_VERSION}" LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED YES)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
    # JUCE's C/C++ visibility settings do not cover Objective-C++ modules.
    # Keep their helpers private so host/plugin JUCE copies cannot mix state.
    set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/juce-version"
    "${CMAKE_CURRENT_SOURCE_DIR}/onda-version"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin-version")

if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "14.0")
    message(FATAL_ERROR "The Onda plugin requires macOS 14.0 or later")
endif()

if(MSVC)
    # Onda::Static is built with the static MSVC runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
include(CTest)

option(ONDA_PLUGIN_BUILD_TESTS "Build the C++ plugin tests" ON)
option(ONDA_PLUGIN_BUILD_VST3_AUDIT "Build the Linux exported-callback auditor" ON)
set(ONDA_PLUGIN_INPUT_CHANNELS "2" CACHE STRING
    "Number of main audio input channels exposed by each VST3")
set(ONDA_PLUGIN_CONTROL_INPUTS "16" CACHE STRING
    "Number of optional mono external control input buses")
set(ONDA_PLUGIN_OUTPUT_CHANNELS "2" CACHE STRING
    "Number of audio output channels exposed by each VST3")
set(ONDA_PLUGIN_PARAMETER_SLOTS "32" CACHE STRING
    "Number of scalar DAW parameter slots exposed by each VST3")

function(validate_channel_count variable minimum)
    if(NOT "${${variable}}" MATCHES "^(0|[1-9][0-9]?)$")
        message(FATAL_ERROR
            "${variable} must be an integer between ${minimum} and 64")
    endif()
    if(${variable} LESS ${minimum} OR ${variable} GREATER 64)
        message(FATAL_ERROR
            "${variable} must be an integer between ${minimum} and 64")
    endif()
endfunction()

validate_channel_count(ONDA_PLUGIN_INPUT_CHANNELS 0)
validate_channel_count(ONDA_PLUGIN_CONTROL_INPUTS 0)
validate_channel_count(ONDA_PLUGIN_OUTPUT_CHANNELS 1)
validate_channel_count(ONDA_PLUGIN_PARAMETER_SLOTS 1)
math(EXPR ONDA_PLUGIN_TOTAL_INPUT_CHANNELS
    "${ONDA_PLUGIN_INPUT_CHANNELS} + ${ONDA_PLUGIN_CONTROL_INPUTS}")
if(ONDA_PLUGIN_TOTAL_INPUT_CHANNELS GREATER 64)
    message(FATAL_ERROR "Main audio and control inputs together must not exceed 64 channels")
endif()

set(ONDA_SDK_ARCHIVE "" CACHE FILEPATH
    "Optional pre-downloaded archive for the pinned Onda release SDK")
set(ONDA_SDK_CHECKSUMS "" CACHE FILEPATH
    "Optional pre-downloaded SHA256SUMS.txt for the pinned Onda release")
set(ONDA_RELEASE_SOURCE_DIR "" CACHE PATH
    "Optional checkout of the pinned Onda release, used for GPU fixtures and font licenses")
set(ONDA_RELEASE_SOURCE_REVISION "" CACHE STRING
    "Expected Onda resource commit for an unpublished SDK; empty verifies the release tag")
set(ONDA_DEVELOPMENT_SOURCE_DIR "" CACHE PATH
    "Use a local Onda checkout and its target/release libraries for development")
set(ONDA_JUCE_ROOT "" CACHE PATH "Optional local JUCE checkout")
set(ONDA_VST3_VALIDATOR "" CACHE FILEPATH
    "Optional Steinberg VST3 validator executable")

message(STATUS
    "Building Onda VST3 ${PROJECT_VERSION} with Onda SDK ${ONDA_VERSION} "
    "(${ONDA_PLUGIN_INPUT_CHANNELS} main inputs, ${ONDA_PLUGIN_CONTROL_INPUTS} control inputs, "
    "${ONDA_PLUGIN_OUTPUT_CHANNELS} outputs)")

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ONDA_SDK_PLATFORM "windows-x64")
    set(ONDA_SDK_EXTENSION "zip")
elseif(APPLE AND (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64"
        OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"))
    set(ONDA_SDK_PLATFORM "macos-arm64")
    set(ONDA_SDK_EXTENSION "tar.xz")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(ONDA_SDK_PLATFORM "linux-x64")
    set(ONDA_SDK_EXTENSION "tar.xz")
else()
    message(FATAL_ERROR
        "No Onda ${ONDA_VERSION} release SDK is available for "
        "${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(ONDA_DEVELOPMENT_SOURCE_DIR)
    get_filename_component(ONDA_DEVELOPMENT_SOURCE_DIR
        "${ONDA_DEVELOPMENT_SOURCE_DIR}" ABSOLUTE)
    set(ONDA_SDK_ROOT "${ONDA_DEVELOPMENT_SOURCE_DIR}")
    set(ONDA_RELEASE_SOURCE_DIR "${ONDA_DEVELOPMENT_SOURCE_DIR}")
    set(Onda_DIR "${ONDA_DEVELOPMENT_SOURCE_DIR}/cmake")
    message(WARNING "Using a local Onda development build; not a release SDK")
else()
    set(ONDA_SDK_ARCHIVE_NAME
        "onda-${ONDA_VERSION}-${ONDA_SDK_PLATFORM}.${ONDA_SDK_EXTENSION}")
    if(ONDA_SDK_ARCHIVE)
        get_filename_component(ONDA_SDK_ARCHIVE "${ONDA_SDK_ARCHIVE}"
            ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    elseif(EXISTS "${CMAKE_BINARY_DIR}/_deps/${ONDA_SDK_ARCHIVE_NAME}")
        set(ONDA_SDK_ARCHIVE
            "${CMAKE_BINARY_DIR}/_deps/${ONDA_SDK_ARCHIVE_NAME}")
    else()
        set(ONDA_SDK_ARCHIVE
            "${CMAKE_BINARY_DIR}/_deps/${ONDA_SDK_ARCHIVE_NAME}")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
        file(DOWNLOAD
            "https://github.com/onda-lang/onda/releases/download/${ONDA_VERSION}/${ONDA_SDK_ARCHIVE_NAME}"
            "${ONDA_SDK_ARCHIVE}"
            STATUS ONDA_SDK_DOWNLOAD_STATUS
            TLS_VERIFY ON
            SHOW_PROGRESS)
        list(GET ONDA_SDK_DOWNLOAD_STATUS 0 ONDA_SDK_DOWNLOAD_CODE)
        if(NOT ONDA_SDK_DOWNLOAD_CODE EQUAL 0)
            list(GET ONDA_SDK_DOWNLOAD_STATUS 1 ONDA_SDK_DOWNLOAD_MESSAGE)
            message(FATAL_ERROR
                "Could not download the Onda ${ONDA_VERSION} release SDK: "
                "${ONDA_SDK_DOWNLOAD_MESSAGE}. For a private release, download "
                "${ONDA_SDK_ARCHIVE_NAME} and SHA256SUMS.txt with GitHub CLI, "
                "then configure with -DONDA_SDK_ARCHIVE and "
                "-DONDA_SDK_CHECKSUMS")
        endif()
    endif()

    if(NOT EXISTS "${ONDA_SDK_ARCHIVE}")
        message(FATAL_ERROR
            "ONDA_SDK_ARCHIVE does not exist: ${ONDA_SDK_ARCHIVE}")
    endif()

    if(ONDA_SDK_CHECKSUMS)
        get_filename_component(ONDA_SDK_CHECKSUMS "${ONDA_SDK_CHECKSUMS}"
            ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    elseif(EXISTS "${CMAKE_BINARY_DIR}/_deps/${ONDA_VERSION}-SHA256SUMS.txt")
        set(ONDA_SDK_CHECKSUMS
            "${CMAKE_BINARY_DIR}/_deps/${ONDA_VERSION}-SHA256SUMS.txt")
    else()
        set(ONDA_SDK_CHECKSUMS
            "${CMAKE_BINARY_DIR}/_deps/${ONDA_VERSION}-SHA256SUMS.txt")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
        file(DOWNLOAD
            "https://github.com/onda-lang/onda/releases/download/${ONDA_VERSION}/SHA256SUMS.txt"
            "${ONDA_SDK_CHECKSUMS}"
            STATUS ONDA_CHECKSUMS_DOWNLOAD_STATUS
            TLS_VERIFY ON)
        list(GET ONDA_CHECKSUMS_DOWNLOAD_STATUS 0 ONDA_CHECKSUMS_DOWNLOAD_CODE)
        if(NOT ONDA_CHECKSUMS_DOWNLOAD_CODE EQUAL 0)
            list(GET ONDA_CHECKSUMS_DOWNLOAD_STATUS 1 ONDA_CHECKSUMS_DOWNLOAD_MESSAGE)
            message(FATAL_ERROR
                "Could not download checksums for Onda ${ONDA_VERSION}: "
                "${ONDA_CHECKSUMS_DOWNLOAD_MESSAGE}. Download SHA256SUMS.txt "
                "with GitHub CLI and configure with -DONDA_SDK_CHECKSUMS")
        endif()
    endif()

    if(NOT EXISTS "${ONDA_SDK_CHECKSUMS}")
        message(FATAL_ERROR
            "ONDA_SDK_CHECKSUMS does not exist: ${ONDA_SDK_CHECKSUMS}")
    endif()
    file(STRINGS "${ONDA_SDK_CHECKSUMS}" ONDA_SDK_CHECKSUM_LINES)
    set(ONDA_SDK_SHA256 "")
    foreach(ONDA_SDK_CHECKSUM_LINE IN LISTS ONDA_SDK_CHECKSUM_LINES)
        if(ONDA_SDK_CHECKSUM_LINE MATCHES "^([0-9A-Fa-f]+)[ \\t]+(.+)$")
            get_filename_component(ONDA_CHECKSUM_ASSET_NAME "${CMAKE_MATCH_2}" NAME)
            if(ONDA_CHECKSUM_ASSET_NAME STREQUAL ONDA_SDK_ARCHIVE_NAME)
                set(ONDA_SDK_SHA256 "${CMAKE_MATCH_1}")
                break()
            endif()
        endif()
    endforeach()
    string(LENGTH "${ONDA_SDK_SHA256}" ONDA_SDK_SHA256_LENGTH)
    if(NOT ONDA_SDK_SHA256_LENGTH EQUAL 64)
        message(FATAL_ERROR
            "${ONDA_SDK_CHECKSUMS} has no valid checksum for "
            "${ONDA_SDK_ARCHIVE_NAME}")
    endif()
    file(SHA256 "${ONDA_SDK_ARCHIVE}" ONDA_SDK_ARCHIVE_SHA256)
    if(NOT ONDA_SDK_ARCHIVE_SHA256 STREQUAL ONDA_SDK_SHA256)
        message(FATAL_ERROR
            "Onda SDK checksum mismatch: expected ${ONDA_SDK_SHA256}, got "
            "${ONDA_SDK_ARCHIVE_SHA256}")
    endif()

    set(ONDA_SDK_EXTRACT_DIR
        "${CMAKE_BINARY_DIR}/_deps/onda-${ONDA_VERSION}-${ONDA_SDK_PLATFORM}")
    set(ONDA_SDK_ROOT
        "${ONDA_SDK_EXTRACT_DIR}/onda-${ONDA_VERSION}-${ONDA_SDK_PLATFORM}")
    include(cmake/OndaSdkArchive.cmake)
    onda_extract_sdk_archive("${ONDA_SDK_ARCHIVE}"
        "${ONDA_SDK_ARCHIVE_SHA256}" "${ONDA_SDK_ROOT}")

    if(NOT ONDA_RELEASE_SOURCE_DIR)
        FetchContent_Declare(
            OndaReleaseSource
            GIT_REPOSITORY https://github.com/onda-lang/onda.git
            GIT_TAG "${ONDA_VERSION}"
            GIT_SHALLOW FALSE
            SOURCE_SUBDIR _resources_only
        )
        FetchContent_MakeAvailable(OndaReleaseSource)
        set(ONDA_RELEASE_SOURCE_DIR "${ondareleasesource_SOURCE_DIR}")
    endif()

    if(ONDA_RELEASE_SOURCE_REVISION)
        string(LENGTH "${ONDA_RELEASE_SOURCE_REVISION}" onda_revision_length)
        if(NOT ONDA_RELEASE_SOURCE_REVISION MATCHES "^[0-9a-f]+$"
                OR NOT onda_revision_length EQUAL 40)
            message(FATAL_ERROR "ONDA_RELEASE_SOURCE_REVISION must be a full commit SHA")
        endif()
        set(onda_resource_ref "${ONDA_RELEASE_SOURCE_REVISION}")
    else()
        set(onda_resource_ref "${ONDA_VERSION}")
    endif()
    execute_process(
        COMMAND git rev-parse "${onda_resource_ref}^{commit}"
        WORKING_DIRECTORY "${ONDA_RELEASE_SOURCE_DIR}"
        OUTPUT_VARIABLE ONDA_RELEASE_TAG_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE ONDA_RELEASE_TAG_GIT_RESULT)
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY "${ONDA_RELEASE_SOURCE_DIR}"
        OUTPUT_VARIABLE ONDA_RELEASE_SOURCE_HEAD
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE ONDA_RELEASE_HEAD_GIT_RESULT)
    if(NOT ONDA_RELEASE_TAG_GIT_RESULT EQUAL 0
            OR NOT ONDA_RELEASE_HEAD_GIT_RESULT EQUAL 0
            OR NOT ONDA_RELEASE_SOURCE_HEAD STREQUAL ONDA_RELEASE_TAG_REVISION)
        message(FATAL_ERROR
            "Onda resources are not at ${onda_resource_ref}: "
            "got ${ONDA_RELEASE_SOURCE_HEAD}")
    endif()

    set(Onda_DIR "${ONDA_SDK_ROOT}/lib/cmake/Onda")
endif()
find_package(Onda CONFIG REQUIRED)
if(APPLE)
    get_target_property(ONDA_STATIC_LIBRARY Onda::Static IMPORTED_LOCATION)
    execute_process(
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-macos-deployment-target.sh"
            "${CMAKE_OSX_DEPLOYMENT_TARGET}" "${ONDA_STATIC_LIBRARY}"
        RESULT_VARIABLE ONDA_SDK_TARGET_RESULT)
    if(NOT ONDA_SDK_TARGET_RESULT EQUAL 0)
        message(FATAL_ERROR
            "The Onda SDK requires a newer macOS version than "
            "${CMAKE_OSX_DEPLOYMENT_TARGET}; rebuild the SDK with "
            "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
endif()

if(ONDA_JUCE_ROOT)
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY "${ONDA_JUCE_ROOT}"
        OUTPUT_VARIABLE JUCE_HEAD
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE JUCE_GIT_RESULT
    )
    if(NOT JUCE_GIT_RESULT EQUAL 0 OR NOT JUCE_HEAD STREQUAL JUCE_REVISION)
        message(FATAL_ERROR
            "JUCE revision mismatch: expected ${JUCE_REVISION}, got ${JUCE_HEAD}")
    endif()
    add_subdirectory("${ONDA_JUCE_ROOT}" "${CMAKE_BINARY_DIR}/juce")
    set(ONDA_JUCE_SOURCE_DIR "${ONDA_JUCE_ROOT}")
else()
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG "${JUCE_REVISION}"
        GIT_SHALLOW FALSE
    )
    FetchContent_MakeAvailable(JUCE)
    set(ONDA_JUCE_SOURCE_DIR "${juce_SOURCE_DIR}")
endif()

# JUCE modules are compiled as sources of each consumer, so consumer warning
# flags otherwise diagnose JUCE and its bundled codecs as if they were ours.
file(GLOB ONDA_JUCE_MODULE_SOURCES
    "${ONDA_JUCE_SOURCE_DIR}/modules/*/*.c"
    "${ONDA_JUCE_SOURCE_DIR}/modules/*/*.cpp"
    "${ONDA_JUCE_SOURCE_DIR}/modules/*/*.m"
    "${ONDA_JUCE_SOURCE_DIR}/modules/*/*.mm")
if(NOT MSVC)
    set_property(SOURCE ${ONDA_JUCE_MODULE_SOURCES} APPEND PROPERTY
        COMPILE_OPTIONS -w)
endif()

include(cmake/OndaVst3Automation.cmake)
include(cmake/OndaJuceXEmbed.cmake)
include(cmake/OndaJuceGuiBasics.cmake)

function(onda_treat_juce_headers_as_system target)
    target_include_directories(${target} SYSTEM PRIVATE
        "${ONDA_JUCE_SOURCE_DIR}/modules")
endfunction()

if(WIN32)
    set(ONDA_GPU_SURFACE_SOURCE src/NativeGpuSurface_windows.cpp)
elseif(APPLE)
    set(ONDA_GPU_SURFACE_SOURCE src/NativeGpuSurface_mac.mm)
else()
    find_package(X11 REQUIRED COMPONENTS Xext)
    set(ONDA_GPU_SURFACE_SOURCE src/NativeGpuSurface_linux.cpp)
endif()

function(onda_add_gpu_view target)
    target_sources(${target} PRIVATE src/GpuView.cpp src/GpuView.h
        src/GpuTextInput.cpp src/NativeGpuSurface.h ${ONDA_GPU_SURFACE_SOURCE})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(${target} PRIVATE X11::X11 X11::Xext)
    elseif(APPLE)
        target_compile_definitions(${target} PRIVATE
            ONDA_GPU_NS_VIEW_CLASS=${target}GpuSurfaceView)
    endif()
endfunction()

add_library(onda_lto_flags INTERFACE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(onda_lto_flags INTERFACE
        $<$<CONFIG:Release>:-flto=auto>)
    target_link_options(onda_lto_flags INTERFACE
        $<$<CONFIG:Release>:-flto=auto>)
else()
    target_link_libraries(onda_lto_flags INTERFACE
        juce::juce_recommended_lto_flags)
endif()

set(ONDA_RELEASE_LICENSE_DIR "${CMAKE_BINARY_DIR}/release-licenses")
file(REMOVE_RECURSE "${ONDA_RELEASE_LICENSE_DIR}")
file(MAKE_DIRECTORY "${ONDA_RELEASE_LICENSE_DIR}")

function(copy_onda_release_license source destination)
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Required release license not found: ${source}")
    endif()
    configure_file("${source}"
        "${ONDA_RELEASE_LICENSE_DIR}/${destination}" COPYONLY)
endfunction()

foreach(ONDA_SDK_LICENSE_PATH IN ITEMS LICENSE THIRD_PARTY_NOTICES.md licenses)
    if(NOT EXISTS "${ONDA_SDK_ROOT}/${ONDA_SDK_LICENSE_PATH}")
        message(FATAL_ERROR
            "Required Onda SDK release material not found: "
            "${ONDA_SDK_ROOT}/${ONDA_SDK_LICENSE_PATH}")
    endif()
endforeach()
file(MAKE_DIRECTORY "${ONDA_RELEASE_LICENSE_DIR}/Onda")
file(COPY
    "${ONDA_SDK_ROOT}/LICENSE"
    "${ONDA_SDK_ROOT}/THIRD_PARTY_NOTICES.md"
    "${ONDA_SDK_ROOT}/licenses"
    DESTINATION "${ONDA_RELEASE_LICENSE_DIR}/Onda")

copy_onda_release_license("${ONDA_JUCE_SOURCE_DIR}/LICENSE.md" "JUCE-LICENSE.md")
copy_onda_release_license(
    "${ONDA_JUCE_SOURCE_DIR}/modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt"
    "VST3-SDK-LICENSE.txt")
copy_onda_release_license(
    "${ONDA_JUCE_SOURCE_DIR}/modules/juce_audio_formats/codecs/flac/Flac Licence.txt"
    "FLAC-LICENSE.txt")
copy_onda_release_license(
    "${ONDA_JUCE_SOURCE_DIR}/modules/juce_audio_formats/codecs/oggvorbis/Ogg Vorbis Licence.txt"
    "Ogg-Vorbis-LICENSE.txt")
copy_onda_release_license(
    "${ONDA_JUCE_SOURCE_DIR}/modules/juce_core/zip/zlib/LICENSE"
    "zlib-LICENSE.txt")
copy_onda_release_license(
    "${ONDA_JUCE_SOURCE_DIR}/modules/juce_graphics/image_formats/pnglib/LICENSE"
    "libpng-LICENSE.txt")
copy_onda_release_license(
    "${ONDA_JUCE_SOURCE_DIR}/modules/juce_graphics/image_formats/jpglib/README"
    "libjpeg-README.txt")
copy_onda_release_license(
    "${ONDA_JUCE_SOURCE_DIR}/modules/juce_graphics/fonts/harfbuzz/COPYING"
    "HarfBuzz-COPYING.txt")
copy_onda_release_license(
    "${ONDA_JUCE_SOURCE_DIR}/modules/juce_graphics/unicode/sheenbidi/LICENSE"
    "SheenBidi-LICENSE.txt")

include(cmake/OndaGpuUiLibrary.cmake)

add_library(onda_plugin_engine STATIC
    src/ParameterAutomation.cpp
    src/ParameterAutomation.h
    src/ParameterMapping.h
    src/ParameterState.h
    src/ScalarValue.h
    src/ControlValue.h
    src/ControlState.h
    src/HostAutomation.h
    src/AudioFile.cpp
    src/AudioFile.h
    src/BufferWaveform.cpp
    src/BufferWaveform.h
    src/Engine.cpp
    src/Engine.h
    src/EnumMetadata.cpp
    src/EnumMetadata.h
    src/EventPayload.cpp
    src/EventPayload.h
    src/FilesystemWatcher.cpp
    src/FilesystemWatcher.h
    src/OndaSdk.cpp
    src/OndaSdk.h
    src/Primitive.h
    src/ParameterRamp.h
    src/ProjectExport.cpp
    src/ProjectExport.h
    src/Product.h
    src/RuntimeLog.h
    src/SpscSlot.h
    src/Worker.cpp
    src/Worker.h
)
target_compile_features(onda_plugin_engine PUBLIC cxx_std_20)
target_include_directories(onda_plugin_engine PUBLIC src)
target_link_libraries(onda_plugin_engine
    PUBLIC Onda::Static
    PRIVATE juce::juce_audio_formats
)
onda_treat_juce_headers_as_system(onda_plugin_engine)
target_compile_definitions(onda_plugin_engine PUBLIC
    JUCE_USE_CURL=0
    ONDA_PLUGIN_INPUT_CHANNELS=${ONDA_PLUGIN_INPUT_CHANNELS}
    ONDA_PLUGIN_CONTROL_INPUTS=${ONDA_PLUGIN_CONTROL_INPUTS}
    ONDA_PLUGIN_OUTPUT_CHANNELS=${ONDA_PLUGIN_OUTPUT_CHANNELS}
    ONDA_PLUGIN_PARAMETER_SLOTS=${ONDA_PLUGIN_PARAMETER_SLOTS}
)
# Keep engine and codec symbols private to each module.
set_target_properties(onda_plugin_engine PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
    target_compile_options(onda_plugin_engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(onda_plugin_engine PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

function(add_onda_plugin target product_name plugin_code is_synth)
    juce_add_plugin(${target}
        COMPANY_NAME "Onda"
        BUNDLE_ID "org.onda-lang.${target}"
        PLUGIN_MANUFACTURER_CODE Onda
        PLUGIN_CODE ${plugin_code}
        PRODUCT_NAME "${product_name}"
        DESCRIPTION "${product_name}"
        FORMATS VST3
        VST3_CATEGORIES ${ARGN}
        IS_SYNTH ${is_synth}
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
    )
    juce_generate_juce_header(${target})
    target_sources(${target} PRIVATE
        src/Editor.cpp
        src/Editor.h
        src/JucePath.h
        src/HostParameter.h
        src/PluginEntry.cpp
        src/Processor.cpp
        src/Processor.h
        src/RunViewHost.cpp
        src/RunViewHost.h
        src/Vst3Automation.cpp
        src/Vst3Automation.h
        src/ScopeCapture.h
        src/StateFormat.h
        src/SpscQueue.h
    )
    onda_add_gpu_view(${target})
    target_include_directories(${target} PRIVATE "${ONDA_SDK_ROOT}/include")
    target_compile_definitions(${target} PRIVATE
        ONDA_GPU_UI_LIBRARY_NAME="${ONDA_GPU_UI_LIBRARY_NAME}")
    set(onda_gpu_resources "$<TARGET_FILE_DIR:${target}_VST3>/../Resources/onda_ui")
    file(GLOB onda_gpu_font_notices CONFIGURE_DEPENDS "${ONDA_RELEASE_SOURCE_DIR}/assets/fonts/OFL-*.txt")
    add_custom_command(TARGET ${target}_VST3 POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${onda_gpu_resources}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${onda_gpu_resources}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ONDA_GPU_UI_LIBRARY}" "${onda_gpu_resources}/${ONDA_GPU_UI_LIBRARY_NAME}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${onda_gpu_font_notices} "${onda_gpu_resources}"
        VERBATIM)
    set_property(TARGET ${target}_VST3 APPEND PROPERTY LINK_DEPENDS
        "${ONDA_GPU_UI_LIBRARY}" ${onda_gpu_font_notices})
    target_link_libraries(${target} PRIVATE
        onda_plugin_engine
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_plugin_client
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
        onda_lto_flags
    )
    onda_treat_juce_headers_as_system(${target})
    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
    )
    target_compile_features(${target} PRIVATE cxx_std_20)
endfunction()

add_onda_plugin(OndaSynth "OndaSynth" Odyn TRUE
    Instrument Synth)
target_compile_definitions(OndaSynth PRIVATE
    ONDA_PLUGIN_PRODUCT_INSTRUMENT=1)

add_onda_plugin(OndaFX "OndaFX" Oefx FALSE
    Fx)
target_compile_definitions(OndaFX PRIVATE
    ONDA_PLUGIN_PRODUCT_EFFECT=1)

if(BUILD_TESTING AND ONDA_PLUGIN_BUILD_TESTS)
    include(cmake/OndaTests.cmake)
endif()
