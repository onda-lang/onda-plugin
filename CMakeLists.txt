cmake_minimum_required(VERSION 3.22)

project(onda-plugin VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
include(CTest)

option(ONDA_PLUGIN_BUILD_TESTS "Build the headless integration tests" ON)
set(ONDA_SDK_ARCHIVE "" CACHE FILEPATH
    "Optional pre-downloaded archive for the pinned Onda release SDK")
set(ONDA_SDK_CHECKSUMS "" CACHE FILEPATH
    "Optional pre-downloaded SHA256SUMS.txt for the pinned Onda release")
set(ONDA_RELEASE_SOURCE_DIR "" CACHE PATH
    "Optional checkout of the pinned Onda release, used only for UI resources")
set(ONDA_JUCE_ROOT "" CACHE PATH "Optional local JUCE checkout")
set(ONDA_VST3_VALIDATOR "" CACHE FILEPATH
    "Optional Steinberg VST3 validator executable")

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
message(STATUS "Building with Onda version: ${ONDA_VERSION}")

set(JUCE_REVISION "7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2")

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
if(NOT EXISTS "${ONDA_SDK_ROOT}/lib/cmake/Onda/OndaConfig.cmake")
    file(REMOVE_RECURSE "${ONDA_SDK_EXTRACT_DIR}")
    file(MAKE_DIRECTORY "${ONDA_SDK_EXTRACT_DIR}")
    file(ARCHIVE_EXTRACT
        INPUT "${ONDA_SDK_ARCHIVE}"
        DESTINATION "${ONDA_SDK_EXTRACT_DIR}")
endif()

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

execute_process(
    COMMAND git rev-parse "${ONDA_VERSION}^{commit}"
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
        "Onda release resources are not at tag ${ONDA_VERSION}: "
        "got ${ONDA_RELEASE_SOURCE_HEAD}")
endif()

set(Onda_DIR "${ONDA_SDK_ROOT}/lib/cmake/Onda")
find_package(Onda CONFIG REQUIRED)

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
else()
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG "${JUCE_REVISION}"
        GIT_SHALLOW FALSE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_binary_data(onda_run_resources
    NAMESPACE OndaRunResources
    HEADER_NAME OndaRunResources.h
    SOURCES
        "${ONDA_RELEASE_SOURCE_DIR}/ui/run/run.html"
        "${ONDA_RELEASE_SOURCE_DIR}/packages/onda_processor_abi/src/param-control.js"
)

add_library(onda_plugin_engine STATIC
    src/AudioFile.cpp
    src/AudioFile.h
    src/Engine.cpp
    src/Engine.h
    src/OndaSdk.cpp
    src/OndaSdk.h
    src/ProjectExport.cpp
    src/ProjectExport.h
    src/Product.h
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
target_compile_definitions(onda_plugin_engine PUBLIC JUCE_USE_CURL=0)
set_target_properties(onda_plugin_engine PROPERTIES
    CXX_EXTENSIONS OFF
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
        NEEDS_WEB_BROWSER TRUE
    )
    juce_generate_juce_header(${target})
    target_sources(${target} PRIVATE
        src/Editor.cpp
        src/Editor.h
        src/JucePath.h
        src/PluginEntry.cpp
        src/Processor.cpp
        src/Processor.h
        src/RunViewHost.cpp
        src/RunViewHost.h
    )
    target_link_libraries(${target} PRIVATE
        onda_plugin_engine
        onda_run_resources
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_gui_extra
        juce::juce_audio_plugin_client
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
    )
    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
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
    add_executable(onda_plugin_tests tests/EngineTests.cpp)
    target_compile_features(onda_plugin_tests PRIVATE cxx_std_20)
    target_link_libraries(onda_plugin_tests PRIVATE
        onda_plugin_engine
        juce::juce_audio_formats
    )
    if(MSVC)
        target_compile_options(onda_plugin_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(onda_plugin_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
    add_test(NAME onda_plugin_tests COMMAND onda_plugin_tests)

    add_executable(onda_processor_tests
        tests/ProcessorTests.cpp
        src/Editor.cpp
        src/Editor.h
        src/JucePath.h
        src/Processor.cpp
        src/Processor.h
        src/RunViewHost.cpp
        src/RunViewHost.h
    )
    target_compile_features(onda_processor_tests PRIVATE cxx_std_20)
    target_link_libraries(onda_processor_tests PRIVATE
        onda_plugin_engine
        onda_run_resources
        juce::juce_audio_processors
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
    target_compile_definitions(onda_processor_tests PRIVATE
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_USE_EXTERNAL_TEMPORARY_SUBPROCESS=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(onda_processor_tests PRIVATE
            juce::pkgconfig_JUCE_BROWSER_LINUX_DEPS)
        target_link_options(onda_processor_tests PRIVATE
            "-Wl,--wrap=pthread_mutex_lock")
        juce_link_with_embedded_linux_subprocess(onda_processor_tests)
    endif()
    if(MSVC)
        target_compile_options(onda_processor_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(onda_processor_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
    add_test(NAME onda_processor_tests COMMAND onda_processor_tests)

    add_executable(onda_vst3_smoke tests/Vst3Smoke.cpp)
    target_compile_features(onda_vst3_smoke PRIVATE cxx_std_20)
    target_link_libraries(onda_vst3_smoke PRIVATE
        juce::juce_audio_processors_headless
        juce::juce_recommended_config_flags
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    target_compile_definitions(onda_vst3_smoke PRIVATE
        JUCE_PLUGINHOST_AU=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_PLUGINHOST_LV2=0
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_USE_CURL=0
    )
    if(MSVC)
        target_compile_options(onda_vst3_smoke PRIVATE /W4 /permissive-)
    else()
        target_compile_options(onda_vst3_smoke PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_dependencies(onda_vst3_smoke
        OndaSynth_VST3
        OndaFX_VST3
    )
    add_test(
        NAME onda_vst3_smoke
        COMMAND onda_vst3_smoke
            "$<TARGET_FILE:OndaSynth_VST3>"
            "$<TARGET_FILE:OndaFX_VST3>"
    )

    if(ONDA_VST3_VALIDATOR)
        if(NOT EXISTS "${ONDA_VST3_VALIDATOR}")
            message(FATAL_ERROR
                "ONDA_VST3_VALIDATOR does not exist: ${ONDA_VST3_VALIDATOR}")
        endif()
        add_test(
            NAME onda_vst3_validator_instrument
            COMMAND "${ONDA_VST3_VALIDATOR}" -q
                "${CMAKE_CURRENT_BINARY_DIR}/OndaSynth_artefacts/$<CONFIG>/VST3/OndaSynth.vst3"
        )
        add_test(
            NAME onda_vst3_validator_effect
            COMMAND "${ONDA_VST3_VALIDATOR}" -q
                "${CMAKE_CURRENT_BINARY_DIR}/OndaFX_artefacts/$<CONFIG>/VST3/OndaFX.vst3"
        )
    endif()
endif()
