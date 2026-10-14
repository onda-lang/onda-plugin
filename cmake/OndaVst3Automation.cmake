# JUCE exposes only queue endpoints. Forward the full queues to our processor
# from a generated wrapper, leaving the pinned dependency checkout unchanged.
set(onda_vst3_original
    "${ONDA_JUCE_SOURCE_DIR}/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${onda_vst3_original}")
file(READ "${onda_vst3_original}" onda_vst3_source)
string(REPLACE "\r\n" "\n" onda_vst3_source "${onda_vst3_source}")
set(onda_vst3_capture_anchor
    "        midiBuffer.clear();\n\n        if (data.inputParameterChanges != nullptr)")
set(onda_vst3_publish_anchor
    "            processParameterChanges (*data.inputParameterChanges);\n\n       #if JucePlugin_WantsMidiInput")
set(onda_vst3_bus_anchor "                toString128 (info.name, bus->getName());")
set(onda_vst3_titles_anchor "        if (details.parameterInfoChanged)\n        {")
set(onda_vst3_endpoint_anchor "                const auto vstParamID = paramQueue->getParameterId();")
set(onda_vst3_controller_write_anchor "                if (! owner.vst3IsPlaying)")
set(onda_vst3_controller_edit_anchor "            performEdit (vstParamId, newValue);")
set(onda_vst3_to_plain_anchor
    "        Vst::ParamValue toPlain (Vst::ParamValue v) const override       { return v; }")
set(onda_vst3_to_normalized_anchor
    "        Vst::ParamValue toNormalized (Vst::ParamValue v) const override  { return v; }")
set(onda_vst3_steps_anchor
    "                return (Steinberg::int32) (0 < numSteps && numSteps < 0x7fffffff ? numSteps - 1 : 0);")
string(LENGTH "${onda_vst3_source}" onda_vst3_source_length)
foreach(anchor IN ITEMS onda_vst3_capture_anchor onda_vst3_publish_anchor
                        onda_vst3_bus_anchor onda_vst3_titles_anchor
                        onda_vst3_endpoint_anchor onda_vst3_controller_write_anchor
                        onda_vst3_controller_edit_anchor
                        onda_vst3_to_plain_anchor onda_vst3_to_normalized_anchor
                        onda_vst3_steps_anchor)
    string(REPLACE "${${anchor}}" "" unmatched "${onda_vst3_source}")
    string(LENGTH "${unmatched}" unmatched_length)
    string(LENGTH "${${anchor}}" anchor_length)
    math(EXPR matches
        "(${onda_vst3_source_length} - ${unmatched_length}) / ${anchor_length}")
    if(NOT matches EQUAL 1)
        message(FATAL_ERROR
            "The pinned JUCE wrapper must contain exactly one ${anchor} (found ${matches})")
    endif()
endforeach()
# Onda publishes managed endpoints atomically and notifies listeners on the
# message thread. Keep JUCE's original handling for bypass and MIDI mappings.
string(REPLACE "${onda_vst3_endpoint_anchor}"
    "${onda_vst3_endpoint_anchor}\n                if (onda::plugin::vst3OwnsParameter (*pluginInstance, vstParamID))\n                    continue;"
    onda_vst3_source "${onda_vst3_source}")
string(REPLACE "${onda_vst3_capture_anchor}"
    "        onda::plugin::ScopedVst3Automation ondaAutomation (*pluginInstance, data);\n\n${onda_vst3_capture_anchor}"
    onda_vst3_source "${onda_vst3_source}")
string(REPLACE "${onda_vst3_publish_anchor}"
    "            processParameterChanges (*data.inputParameterChanges);\n\n        ondaAutomation.publishTargets();\n\n       #if JucePlugin_WantsMidiInput"
    onda_vst3_source "${onda_vst3_source}")
string(REPLACE "${onda_vst3_bus_anchor}"
    "                toString128 (info.name, onda::plugin::vst3BusName (*pluginInstance, dir == Vst::kInput, index));"
    onda_vst3_source "${onda_vst3_source}")
# Onda patch changes have no JUCE program parameter. Still invalidate the
# host's value cache after plugin-owned defaults reach the controller.
string(REPLACE "${onda_vst3_titles_anchor}"
    "        if (details.programChanged)\n            flags |= Vst::kParamValuesChanged;\n\n${onda_vst3_titles_anchor}\n            flags |= Vst::kIoTitlesChanged;"
    onda_vst3_source "${onda_vst3_source}")
# Use the same numeric mapping for controller conversions and DSP.
string(REPLACE "${onda_vst3_to_plain_anchor}"
    "        Vst::ParamValue toPlain (Vst::ParamValue v) const override       { return onda::plugin::vst3NormalizedToPlain (param, v); }"
    onda_vst3_source "${onda_vst3_source}")
string(REPLACE "${onda_vst3_to_normalized_anchor}"
    "        Vst::ParamValue toNormalized (Vst::ParamValue v) const override  { return onda::plugin::vst3PlainToNormalized (param, v); }"
    onda_vst3_source "${onda_vst3_source}")
# VST3 counts intervals, whereas JUCE counts values and reserves INT_MAX for
# continuous parameters. Publish compiled discrete counts without that sentinel.
string(REPLACE "${onda_vst3_steps_anchor}"
    "                return onda::plugin::vst3ParameterStepCount (param, (Steinberg::int32) (0 < numSteps && numSteps < 0x7fffffff ? numSteps - 1 : 0));"
    onda_vst3_source "${onda_vst3_source}")
# Host endpoints synchronize controller presentation without writing rounded
# values back to DSP or reporting new plugin edits to the host.
string(REPLACE "${onda_vst3_controller_write_anchor}"
    "                if (! owner.vst3IsPlaying && ! onda::plugin::vst3IsHostParameterNotification (&param))"
    onda_vst3_source "${onda_vst3_source}")
string(REPLACE "${onda_vst3_controller_edit_anchor}"
    "            if (! onda::plugin::vst3IsHostParameterNotification (audioProcessor->getParamForVSTParamID (vstParamId)))\n                performEdit (vstParamId, newValue);"
    onda_vst3_source "${onda_vst3_source}")
string(PREPEND onda_vst3_source "#include \"Vst3Automation.h\"\n")
set(onda_vst3_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/juce_audio_plugin_client_VST3.cpp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${onda_vst3_generated}" CONTENT "${onda_vst3_source}" @ONLY)
get_target_property(onda_vst3_sources juce_audio_plugin_client_VST3 INTERFACE_SOURCES)
list(FIND onda_vst3_sources "${onda_vst3_original}" onda_vst3_index)
if(onda_vst3_index LESS 0)
    message(FATAL_ERROR "Could not find JUCE's VST3 wrapper compilation unit")
endif()
list(REMOVE_ITEM onda_vst3_sources "${onda_vst3_original}")
list(APPEND onda_vst3_sources "${onda_vst3_generated}")
set_property(TARGET juce_audio_plugin_client_VST3 PROPERTY INTERFACE_SOURCES "${onda_vst3_sources}")
