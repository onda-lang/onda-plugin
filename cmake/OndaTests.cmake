# Releases run core/native; optional groups and channel layouts are available
# through manual CI runs and locally.
function(onda_add_test name group)
    add_test(NAME ${name} COMMAND ${ARGN})
    set_tests_properties(${name} PROPERTIES LABELS "${group}" TIMEOUT 180)
endfunction()

if(ONDA_PLUGIN_BUILD_BROWSER_TESTS)
    find_program(ONDA_NODE_EXECUTABLE NAMES node nodejs REQUIRED)
    onda_add_test(onda_editor_bridge_tests browser
        "${ONDA_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/EditorBridgeTests.mjs")
    onda_add_test(onda_run_view_parameter_tests browser
        "${ONDA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/RunViewParameterTests.mjs"
        "${onda_run_view_html}"
        "${ONDA_RELEASE_SOURCE_DIR}/ui/number-input.js"
        "${ONDA_RELEASE_SOURCE_DIR}/packages/onda_processor_abi/src/param-control.js")
endif()

add_executable(onda_example_tests tests/ExampleTests.cpp)
target_compile_features(onda_example_tests PRIVATE cxx_std_20)
target_compile_definitions(onda_example_tests PRIVATE
    ONDA_PLUGIN_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
target_link_libraries(onda_example_tests PRIVATE onda_plugin_engine)
if(MSVC)
    target_compile_options(onda_example_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(onda_example_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()
if(ONDA_PLUGIN_INPUT_CHANNELS EQUAL 2 AND
        ONDA_PLUGIN_OUTPUT_CHANNELS EQUAL 2)
    onda_add_test(onda_example_tests core onda_example_tests)
endif()

add_executable(onda_plugin_tests tests/EngineTests.cpp)
target_compile_features(onda_plugin_tests PRIVATE cxx_std_20)
target_link_libraries(onda_plugin_tests PRIVATE
    onda_plugin_engine
    juce::juce_audio_formats
)
onda_treat_juce_headers_as_system(onda_plugin_tests)
if(MSVC)
    target_compile_options(onda_plugin_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(onda_plugin_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()
if(ONDA_PLUGIN_INPUT_CHANNELS EQUAL 2 AND
        ONDA_PLUGIN_OUTPUT_CHANNELS EQUAL 2)
    onda_add_test(onda_plugin_tests core onda_plugin_tests --group core)
    onda_add_test(onda_filesystem_tests native onda_plugin_tests --group native)
    # Ancestor moves and POSIX symlink replacement are excluded on Windows.
    if(NOT WIN32)
        onda_add_test(onda_filesystem_extended_tests extended onda_plugin_tests --group extended)
    endif()
endif()

juce_add_console_app(onda_processor_tests)
set_target_properties(onda_processor_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_sources(onda_processor_tests PRIVATE
    tests/ProcessorTests.cpp
    tests/NativeEditorTests.cpp
    tests/RealtimeAudit.cpp
    tests/RealtimeAuditTests.cpp
    tests/BypassTests.cpp
    tests/BlockProcessingTests.cpp
    tests/AutomationSeedingTests.cpp
    tests/ScratchHandoffTests.cpp
    tests/ControlInputTests.cpp
    tests/ControlAutomationTests.cpp
    tests/ControlRenderingTests.cpp
    tests/InputControlEditorTests.cpp
    tests/UnrangedParameterTests.cpp
    tests/ParameterLifecycleTests.cpp
    tests/ParameterDefaultTests.cpp
    src/Editor.cpp
    src/Editor.h
    src/JucePath.h
    src/HostParameter.h
    src/Processor.cpp
    src/Processor.h
    src/RunViewHost.cpp
    src/RunViewHost.h
    src/Vst3Automation.cpp
    src/Vst3Automation.h
    src/SpscQueue.h
)
target_compile_features(onda_processor_tests PRIVATE cxx_std_20)
if(NOT MSVC)
    set_source_files_properties(tests/RealtimeAuditTests.cpp PROPERTIES
        COMPILE_OPTIONS "-fno-builtin;$<$<CXX_COMPILER_ID:Clang,AppleClang>:-fsized-deallocation>")
endif()
target_link_libraries(onda_processor_tests PRIVATE
    onda_plugin_engine
    onda_run_resources
    juce::juce_audio_processors
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
onda_treat_juce_headers_as_system(onda_processor_tests)
target_compile_definitions(onda_processor_tests PRIVATE
    JUCE_WEB_BROWSER=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=0
    ONDA_TEST_SYNTH_MODULE="$<TARGET_FILE:OndaSynth_VST3>"
    ONDA_TEST_EFFECT_MODULE="$<TARGET_FILE:OndaFX_VST3>"
    JUCE_USE_CURL=0
    JUCE_USE_EXTERNAL_TEMPORARY_SUBPROCESS=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
)
if(WIN32)
    target_link_libraries(onda_processor_tests PRIVATE juce::juce_webview2)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(onda_processor_tests PRIVATE
        juce::pkgconfig_JUCE_BROWSER_LINUX_DEPS)
    target_link_options(onda_processor_tests PRIVATE
        "-Wl,--wrap=pthread_mutex_lock"
        "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free"
        "-Wl,--wrap=aligned_alloc,--wrap=posix_memalign,--wrap=memalign,--wrap=valloc,--wrap=pvalloc"
        "-Wl,--wrap=reallocarray,--wrap=strdup,--wrap=strndup"
        "-Wl,--wrap=free_sized,--wrap=free_aligned_sized")
    juce_link_with_embedded_linux_subprocess(onda_processor_tests)
endif()
if(MSVC)
    target_compile_options(onda_processor_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(onda_processor_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # The test intentionally pairs malloc/free in replacement
        # new/delete operators so it can audit real-time allocations.
        target_compile_options(onda_processor_tests PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-mismatched-new-delete>)
    endif()
endif()
if(ONDA_PLUGIN_INPUT_CHANNELS EQUAL 2 AND
        ONDA_PLUGIN_OUTPUT_CHANNELS EQUAL 2)
    onda_add_test(onda_processor_tests core onda_processor_tests --group core)
    onda_add_test(onda_native_editor_tests native onda_processor_tests --group native)
    onda_add_test(onda_processor_stress_tests extended onda_processor_tests --group extended)
else()
    onda_add_test(onda_realtime_audit_instrumentation_tests core onda_processor_tests RealtimeAuditInstrumentation)
    onda_add_test(onda_bypass_transition_tests core onda_processor_tests BypassTransitions)
    onda_add_test(onda_callback_processing_tests core onda_processor_tests CallbackProcessing)
    onda_add_test(onda_callback_smoothing_tests core onda_processor_tests CallbackSmoothing)
    onda_add_test(onda_control_input_tests core onda_processor_tests ControlInputs)
    onda_add_test(onda_input_control_editor_tests core onda_processor_tests InputControlEditor)
    onda_add_test(onda_unranged_parameter_tests core onda_processor_tests UnrangedParameters)
    onda_add_test(onda_control_automation_tests core onda_processor_tests ControlAutomation)
    onda_add_test(onda_control_rendering_tests core onda_processor_tests ControlRendering)
    onda_add_test(onda_new_control_default_tests core onda_processor_tests NewControlDefaults)
    onda_add_test(onda_parameter_endpoint_precision_tests core onda_processor_tests ParameterEndpointPrecision)
    onda_add_test(onda_parameter_default_precision_tests core onda_processor_tests ParameterDefaultPrecision)
    onda_add_test(onda_control_endpoint_precision_tests core onda_processor_tests ControlEndpointPrecision)
    onda_add_test(onda_control_default_precision_tests core onda_processor_tests ControlDefaultPrecision)
    onda_add_test(onda_seed_notification_thread_tests core onda_processor_tests SeedNotificationThreads)
    onda_add_test(onda_parameter_default_notification_tests core onda_processor_tests ParameterDefaultNotifications)
    onda_add_test(onda_automation_after_unload_tests core onda_processor_tests AutomationAfterUnload)
    onda_add_test(onda_automation_flush_after_unload_tests core onda_processor_tests AutomationFlushAfterUnload)
    onda_add_test(onda_automation_flush_after_fault_tests core onda_processor_tests AutomationFlushAfterFault)
    onda_add_test(onda_automation_echo_work_tests core onda_processor_tests AutomationEchoWork)
    onda_add_test(onda_unloaded_endpoint_precision_tests core onda_processor_tests UnloadedEndpointPrecision)
    onda_add_test(onda_unloaded_default_precision_tests core onda_processor_tests UnloadedDefaultPrecision)
endif()

add_executable(onda_vst3_smoke
    tests/Vst3Smoke.cpp
    tests/Vst3AutomationTests.cpp
)
target_include_directories(onda_vst3_smoke PRIVATE src)
target_compile_features(onda_vst3_smoke PRIVATE cxx_std_20)
target_link_libraries(onda_vst3_smoke PRIVATE
    juce::juce_audio_processors_headless
    juce::juce_recommended_config_flags
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
onda_treat_juce_headers_as_system(onda_vst3_smoke)
target_compile_definitions(onda_vst3_smoke PRIVATE
    ONDA_PLUGIN_VERSION="${PROJECT_VERSION}"
    ONDA_PLUGIN_INPUT_CHANNELS=${ONDA_PLUGIN_INPUT_CHANNELS}
    ONDA_PLUGIN_CONTROL_INPUTS=${ONDA_PLUGIN_CONTROL_INPUTS}
    ONDA_PLUGIN_OUTPUT_CHANNELS=${ONDA_PLUGIN_OUTPUT_CHANNELS}
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_MODAL_LOOPS_PERMITTED=1
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
set(onda_vst3_smoke_command
    onda_vst3_smoke
    "$<TARGET_FILE:OndaSynth_VST3>"
    "$<TARGET_FILE:OndaFX_VST3>"
)
onda_add_test(onda_vst3_smoke core ${onda_vst3_smoke_command})
set_tests_properties(onda_vst3_smoke PROPERTIES ENVIRONMENT_MODIFICATION
    "LD_PRELOAD=unset:;ONDA_REQUIRE_VST3_AUDIT=unset:")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ONDA_PLUGIN_BUILD_VST3_AUDIT)
    add_library(onda_vst3_realtime_audit SHARED tests/Vst3RealtimeAudit.cpp)
    target_compile_features(onda_vst3_realtime_audit PRIVATE cxx_std_20)
    # This library is preloaded. Access its TLS without re-entering the
    # loader, which can call our allocation interceptors while updating TLS.
    target_compile_options(onda_vst3_realtime_audit PRIVATE
        -ftls-model=initial-exec -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    target_link_libraries(onda_vst3_realtime_audit PRIVATE ${CMAKE_DL_LIBS})
    add_executable(onda_vst3_realtime_audit_tests tests/Vst3RealtimeAuditTests.cpp)
    target_compile_features(onda_vst3_realtime_audit_tests PRIVATE cxx_std_20)
    # These tests deliberately observe allocator calls, including pairs
    # that the compiler could otherwise eliminate as unused allocations.
    target_compile_options(onda_vst3_realtime_audit_tests PRIVATE
        -fno-builtin $<$<CXX_COMPILER_ID:Clang,AppleClang>:-fsized-deallocation>
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    target_link_libraries(onda_vst3_realtime_audit_tests PRIVATE
        onda_vst3_realtime_audit Threads::Threads ${CMAKE_DL_LIBS})
    add_dependencies(onda_vst3_realtime_audit_tests OndaSynth_VST3)
    onda_add_test(onda_vst3_realtime_audit_tests audit
        onda_vst3_realtime_audit_tests "$<TARGET_FILE:OndaSynth_VST3>")
    onda_add_test(onda_vst3_realtime audit ${onda_vst3_smoke_command})
    # Validate the measuring tool first. A broken auditor must fail its own
    # test and prevent a misleading verdict about plugin callbacks.
    set_tests_properties(onda_vst3_realtime_audit_tests PROPERTIES
        FIXTURES_SETUP onda_vst3_auditor)
    set_tests_properties(onda_vst3_realtime PROPERTIES
        FIXTURES_REQUIRED onda_vst3_auditor)
    set_tests_properties(onda_vst3_realtime onda_vst3_realtime_audit_tests PROPERTIES ENVIRONMENT
        "LD_PRELOAD=$<TARGET_FILE:onda_vst3_realtime_audit>;ONDA_REQUIRE_VST3_AUDIT=1")
endif()

if(ONDA_VST3_VALIDATOR)
    if(NOT EXISTS "${ONDA_VST3_VALIDATOR}")
        message(FATAL_ERROR
            "ONDA_VST3_VALIDATOR does not exist: ${ONDA_VST3_VALIDATOR}")
    endif()
    onda_add_test(onda_vst3_validator_instrument core
        "${ONDA_VST3_VALIDATOR}" -q
            "${CMAKE_CURRENT_BINARY_DIR}/OndaSynth_artefacts/$<CONFIG>/VST3/OndaSynth.vst3"
    )
    onda_add_test(onda_vst3_validator_effect core
        "${ONDA_VST3_VALIDATOR}" -q
            "${CMAKE_CURRENT_BINARY_DIR}/OndaFX_artefacts/$<CONFIG>/VST3/OndaFX.vst3"
    )
endif()
add_dependencies(onda_processor_tests OndaSynth_VST3 OndaFX_VST3)
