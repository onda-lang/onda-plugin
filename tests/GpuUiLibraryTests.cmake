cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED TEST_DIRECTORY OR TEST_DIRECTORY STREQUAL "")
    message(FATAL_ERROR "TEST_DIRECTORY is required")
endif()
if(NOT DEFINED TEST_GENERATOR)
    set(TEST_GENERATOR Ninja)
endif()
set(scratch "${TEST_DIRECTORY}")
file(REMOVE_RECURSE "${scratch}")
foreach(sdk IN ITEMS A B)
    file(MAKE_DIRECTORY "${scratch}/${sdk}/include"
        "${scratch}/${sdk}/target/release" "${scratch}/${sdk}/lib")
    file(WRITE "${scratch}/${sdk}/include/onda_ui.h" "")
    file(WRITE "${scratch}/${sdk}/target/release/libonda_ui.test" "")
    file(WRITE "${scratch}/${sdk}/lib/libonda_ui.test" "")
endforeach()
file(WRITE "${scratch}/override.test" "")
file(WRITE "${scratch}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(GpuUiLibrarySelection LANGUAGES NONE)
set(CMAKE_SHARED_LIBRARY_PREFIX lib)
set(CMAKE_SHARED_LIBRARY_SUFFIX .test)
include("${UI_LIBRARY_CONFIGURATION}")
file(WRITE "${CMAKE_BINARY_DIR}/selected.txt" "${ONDA_GPU_UI_LIBRARY}")
]=])

function(expect_library sdk development expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${scratch}/source" -B "${scratch}/build"
            -G "${TEST_GENERATOR}"
            "-DUI_LIBRARY_CONFIGURATION=${CMAKE_CURRENT_LIST_DIR}/../cmake/OndaGpuUiLibrary.cmake"
            "-DONDA_SDK_ROOT=${scratch}/${sdk}"
            "-DONDA_DEVELOPMENT_SOURCE_DIR=${development}" ${ARGN}
        COMMAND_ERROR_IS_FATAL ANY OUTPUT_QUIET)
    file(READ "${scratch}/build/selected.txt" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Expected UI library ${expected}, got ${actual}")
    endif()
endfunction()

# Reconfigure the same build across development checkouts and release SDKs.
expect_library(A "${scratch}/A" "${scratch}/A/target/release/libonda_ui.test")
expect_library(B "${scratch}/B" "${scratch}/B/target/release/libonda_ui.test")
expect_library(A "" "${scratch}/A/lib/libonda_ui.test")
expect_library(B "" "${scratch}/B/lib/libonda_ui.test")
# Explicit overrides survive SDK changes; clearing one resumes automatic selection.
expect_library(A "" "${scratch}/override.test"
    "-DONDA_GPU_UI_LIBRARY=${scratch}/override.test")
expect_library(B "${scratch}/B" "${scratch}/override.test")
expect_library(B "${scratch}/B" "${scratch}/B/target/release/libonda_ui.test"
    "-DONDA_GPU_UI_LIBRARY=")
file(REMOVE_RECURSE "${scratch}")
