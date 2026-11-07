cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/OndaSdkArchive.cmake")

if(NOT DEFINED TEST_DIRECTORY OR TEST_DIRECTORY STREQUAL "")
    message(FATAL_ERROR "TEST_DIRECTORY is required")
endif()
if(NOT DEFINED TEST_GENERATOR)
    set(TEST_GENERATOR Ninja)
endif()
set(scratch "${TEST_DIRECTORY}")
file(REMOVE_RECURSE "${scratch}")
set(source_root "${scratch}/source/onda-sdk")
set(config_path "lib/cmake/Onda/OndaConfig.cmake")
file(MAKE_DIRECTORY "${source_root}/lib/cmake/Onda")
foreach(revision IN ITEMS A B)
    file(WRITE "${source_root}/${config_path}" "# SDK ${revision}\n")
    file(WRITE "${source_root}/only-${revision}" "${revision}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${scratch}/${revision}.zip"
            --format=zip onda-sdk
        WORKING_DIRECTORY "${scratch}/source"
        COMMAND_ERROR_IS_FATAL ANY)
    file(SHA256 "${scratch}/${revision}.zip" sha256_${revision})
    file(REMOVE "${source_root}/only-${revision}")
endforeach()

function(expect_revision sdk_root revision)
    file(READ "${sdk_root}/${config_path}" actual)
    if(NOT actual STREQUAL "# SDK ${revision}\n"
            OR NOT EXISTS "${sdk_root}/only-${revision}")
        message(FATAL_ERROR "Expected SDK ${revision} at ${sdk_root}")
    endif()
endfunction()

set(sdk_root "${scratch}/extracted/onda-sdk")
onda_extract_sdk_archive("${scratch}/A.zip" "${sha256_A}" "${sdk_root}")
expect_revision("${sdk_root}" A)

# Build outputs must follow a replaced SDK even when its archive predates them.
file(WRITE "${scratch}/consumer/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(SdkArchiveConsumer LANGUAGES NONE)
add_custom_command(OUTPUT "${OUTPUT_FILE}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${SDK_CONFIG}" "${OUTPUT_FILE}"
    DEPENDS "${SDK_CONFIG}" VERBATIM)
add_custom_target(consume_sdk ALL DEPENDS "${OUTPUT_FILE}")
]=])
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${scratch}/consumer" -B "${scratch}/build"
        -G "${TEST_GENERATOR}" "-DSDK_CONFIG=${sdk_root}/${config_path}"
        "-DOUTPUT_FILE=${scratch}/selected-sdk.txt"
    COMMAND_ERROR_IS_FATAL ANY OUTPUT_QUIET)
function(expect_consumer revision)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${scratch}/build"
        COMMAND_ERROR_IS_FATAL ANY OUTPUT_QUIET)
    file(READ "${scratch}/selected-sdk.txt" actual)
    if(NOT actual STREQUAL "# SDK ${revision}\n")
        message(FATAL_ERROR "SDK consumer did not rebuild against ${revision}")
    endif()
endfunction()
expect_consumer(A)

# An unchanged archive must reuse the extracted SDK.
file(WRITE "${sdk_root}/sentinel" "retained")
onda_extract_sdk_archive("${scratch}/A.zip" "${sha256_A}" "${sdk_root}")
if(NOT EXISTS "${sdk_root}/sentinel")
    message(FATAL_ERROR "An unchanged SDK archive was extracted again")
endif()

# A different archive with the same root must replace all previous contents.
onda_extract_sdk_archive("${scratch}/B.zip" "${sha256_B}" "${sdk_root}")
expect_revision("${sdk_root}" B)
expect_consumer(B)
if(EXISTS "${sdk_root}/only-A" OR EXISTS "${sdk_root}/sentinel")
    message(FATAL_ERROR "Replacing the SDK retained stale files")
endif()

# Matching archive identity cannot make an incomplete extraction reusable.
file(REMOVE "${sdk_root}/${config_path}")
onda_extract_sdk_archive("${scratch}/B.zip" "${sha256_B}" "${sdk_root}")
expect_revision("${sdk_root}" B)

# A preexisting config alone cannot establish which archive was extracted.
set(unverified_root "${scratch}/unverified/onda-sdk")
file(MAKE_DIRECTORY "${unverified_root}/lib/cmake/Onda")
file(WRITE "${unverified_root}/${config_path}" "# Unknown SDK\n")
onda_extract_sdk_archive("${scratch}/B.zip" "${sha256_B}" "${unverified_root}")
expect_revision("${unverified_root}" B)
file(REMOVE_RECURSE "${scratch}")
