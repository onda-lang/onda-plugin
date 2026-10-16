# Adapt the pinned view's format policy to the plugin's registered decoders.
# Keep the shared card rendering and drag lifecycle owned by Onda.
set(onda_buffers_original "${ONDA_RELEASE_SOURCE_DIR}/ui/run/buffers.js")
file(READ "${onda_buffers_original}" onda_buffers_source)
set(onda_buffers_policy "const isBufferFile = name => /\\.(wav|ondabuffer)$/i.test(name);")
string(FIND "${onda_buffers_source}" "${onda_buffers_policy}" onda_buffers_policy_position)
if(onda_buffers_policy_position EQUAL -1)
    message(FATAL_ERROR "Could not locate pinned Onda buffer format policy")
endif()
string(REPLACE "${onda_buffers_policy}"
    "const isBufferFile = name => globalThis.__hostBridge.isBufferFile(name);"
    onda_buffers_source "${onda_buffers_source}")
set(onda_buffers_script "${CMAKE_CURRENT_BINARY_DIR}/generated/run-view/buffers.js")
file(CONFIGURE OUTPUT "${onda_buffers_script}" CONTENT "${onda_buffers_source}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${onda_buffers_original}")
