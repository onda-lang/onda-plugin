# Keep the pinned view's host-value fixes local and reviewable. Patch a build
# copy so dependency checkouts and other Onda consumers remain untouched.
find_package(Git REQUIRED)
set(onda_run_view_source "${ONDA_RELEASE_SOURCE_DIR}/ui/run/run.html")
set(onda_run_view_patch
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/onda-run-view-parameter-values.patch")
set(onda_run_view_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/onda-run-view")
set(onda_run_view_html "${onda_run_view_directory}/run.html")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${onda_run_view_source}" "${onda_run_view_patch}")
file(MAKE_DIRECTORY "${onda_run_view_directory}")
configure_file("${onda_run_view_source}" "${onda_run_view_html}" COPYONLY)
# Prevent git's containing-checkout path filter from silently skipping the copy.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}"
        "${GIT_EXECUTABLE}" apply --unsafe-paths
        --directory "${onda_run_view_directory}" "${onda_run_view_patch}"
    WORKING_DIRECTORY "${onda_run_view_directory}"
    RESULT_VARIABLE onda_run_view_patch_result
    ERROR_VARIABLE onda_run_view_patch_error)
if(NOT onda_run_view_patch_result EQUAL 0)
    message(FATAL_ERROR "Could not apply the Onda run-view fixes: ${onda_run_view_patch_error}")
endif()
