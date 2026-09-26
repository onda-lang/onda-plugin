file(READ "${ONDA_RELEASE_SOURCE_DIR}/ui/run/run.html" ONDA_RUN_VIEW_HTML)
string(REPLACE "\r\n" "\n" ONDA_RUN_VIEW_HTML "${ONDA_RUN_VIEW_HTML}")

function(onda_replace_run_view old new)
    string(FIND "${ONDA_RUN_VIEW_HTML}" "${old}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "The pinned Onda run view no longer matches the scroll layout")
    endif()
    string(REPLACE "${old}" "${new}" ONDA_RUN_VIEW_HTML "${ONDA_RUN_VIEW_HTML}")
    set(ONDA_RUN_VIEW_HTML "${ONDA_RUN_VIEW_HTML}" PARENT_SCOPE)
endfunction()

onda_replace_run_view(
    [[        height: 100vh;
        min-height: 0;
        padding: 0;
        overflow: hidden;]]
    [[        min-height: 100vh;
        padding: 0;]])
onda_replace_run_view(
    [[        padding: 18px;
        overflow-x: hidden;
        overflow-y: auto;]]
    [[        padding: 18px;]])
onda_replace_run_view(
    [[      .midi-keyboard {
        position: relative;]]
    [[      .midi-keyboard {
        position: sticky;
        bottom: 0;]])
onda_replace_run_view(
    [[const shellNode = document.querySelector(".shell");]]
    [[const scrollNode = document.scrollingElement;]])
onda_replace_run_view([[shellNode.scrollTop]] [[scrollNode.scrollTop]])
onda_replace_run_view(
    [[shellNode.addEventListener("scroll", scheduleScrollViewState, { passive: true });]]
    [[window.addEventListener("scroll", scheduleScrollViewState, { passive: true });]])

set(ONDA_EMBEDDED_RUN_VIEW_DIR "${CMAKE_CURRENT_BINARY_DIR}/embedded-run-view")
file(MAKE_DIRECTORY "${ONDA_EMBEDDED_RUN_VIEW_DIR}")
set(ONDA_EMBEDDED_RUN_VIEW "${ONDA_EMBEDDED_RUN_VIEW_DIR}/run.html")
if(EXISTS "${ONDA_EMBEDDED_RUN_VIEW}")
    file(READ "${ONDA_EMBEDDED_RUN_VIEW}" ONDA_PREVIOUS_RUN_VIEW_HTML)
endif()
if(NOT ONDA_RUN_VIEW_HTML STREQUAL ONDA_PREVIOUS_RUN_VIEW_HTML)
    file(WRITE "${ONDA_EMBEDDED_RUN_VIEW}" "${ONDA_RUN_VIEW_HTML}")
endif()
