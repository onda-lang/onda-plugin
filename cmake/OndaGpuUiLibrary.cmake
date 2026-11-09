set(ONDA_GPU_UI_LIBRARY "" CACHE FILEPATH
    "Optional override for the selected SDK's shared onda_ui renderer")
if(NOT ONDA_GPU_UI_LIBRARY)
    if(ONDA_DEVELOPMENT_SOURCE_DIR)
        set(onda_ui_library_dir "${ONDA_DEVELOPMENT_SOURCE_DIR}/target/release")
    else()
        set(onda_ui_library_dir "${ONDA_SDK_ROOT}/lib")
    endif()
    set(ONDA_GPU_UI_LIBRARY
        "${onda_ui_library_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}onda_ui${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()
if(NOT EXISTS "${ONDA_SDK_ROOT}/include/onda_ui.h" OR NOT EXISTS "${ONDA_GPU_UI_LIBRARY}")
    message(FATAL_ERROR "The Onda SDK must include onda_ui.h and the onda_ui shared library; development checkouts need cargo build --release -p onda_ui --features ffi")
endif()
get_filename_component(ONDA_GPU_UI_LIBRARY_NAME "${ONDA_GPU_UI_LIBRARY}" NAME)
if(WIN32)
    # LoadLibrary accepts explicit extensions; keep support code out of DAW DLL scans.
    set(ONDA_GPU_UI_LIBRARY_NAME "onda_ui.bin")
endif()
