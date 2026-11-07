# sdk_root names the archive's top-level directory below its extraction cache.
# The caller verifies archive_sha256 against the selected SDK's checksums first.
function(onda_extract_sdk_archive archive archive_sha256 sdk_root)
    get_filename_component(extract_dir "${sdk_root}" DIRECTORY)
    set(config_file "${sdk_root}/lib/cmake/Onda/OndaConfig.cmake")
    set(digest_file "${extract_dir}/.archive-sha256")
    set(extracted_sha256 "")
    if(EXISTS "${digest_file}")
        file(STRINGS "${digest_file}" extracted_sha256 LIMIT_COUNT 1)
    endif()
    if(extracted_sha256 STREQUAL archive_sha256
            AND EXISTS "${config_file}")
        return()
    endif()

    file(REMOVE_RECURSE "${extract_dir}")
    file(MAKE_DIRECTORY "${extract_dir}")
    # Archive timestamps may predate existing consumer objects and binaries.
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${extract_dir}" TOUCH)
    if(NOT EXISTS "${config_file}")
        message(FATAL_ERROR "Onda SDK archive is missing ${config_file}")
    endif()
    file(WRITE "${digest_file}" "${archive_sha256}\n")
endfunction()
