#include "JucePath.h"
#include "ParameterRamp.h"
#include "StateFormat.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"
#include "Vst3TestSupport.h"
#include "Vst3AutomationTests.h"
#include "ControlInputTestSupport.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>

#ifndef ONDA_PLUGIN_VERSION
#error "ONDA_PLUGIN_VERSION must match the version embedded by JUCE"
#endif

namespace {

struct ProductExpectation {
  const char *name;
  bool dryFallback;
  const char *componentCid;
  const char *controllerCid;
};

[[nodiscard]] bool check(const bool condition, const juce::String &message) {
  if (condition)
    return true;
  std::cerr << message << '\n';
  return false;
}

[[nodiscard]] bool checkBundleLookup() {
  const juce::File root{onda::plugin::pathToJuce(testTemporaryRoot())};
  const auto bundle = root.getChildFile("Lookup.vst3");
  const auto module = bundle.getChildFile("Contents/x86_64-win/Lookup.vst3");
  if (!check(module.getParentDirectory().createDirectory().wasOk() &&
                 module.replaceWithText("test module"),
             "Could not create the Windows bundle lookup fixture"))
    return false;
  return check(
      vst3BundleForModule(module) == bundle && vst3BundleForModule(bundle) == bundle &&
          vst3BundleForModule(root.getChildFile("missing.bin")) == juce::File{},
      "VST3 bundle lookup did not resolve the bundle directory");
}

[[nodiscard]] bool checkMetadata(const juce::File &bundle,
                                 const ProductExpectation &expected) {
  const auto moduleInfo = bundle.getChildFile("Contents")
                              .getChildFile("Resources")
                              .getChildFile("moduleinfo.json");
  if (!check(moduleInfo.existsAsFile(),
             "Missing VST3 moduleinfo.json in " + bundle.getFullPathName())) {
    return false;
  }

  const auto contents = moduleInfo.loadFileAsString();
  return check(contents.contains("\"Version\": \"" ONDA_PLUGIN_VERSION "\""),
               juce::String(expected.name) +
                   " module version does not match plugin-version") &&
         check(contents.contains("\"CID\": \"" +
                                 juce::String(expected.componentCid) + "\""),
               juce::String(expected.name) +
                   " component CID changed unexpectedly") &&
         check(contents.contains("\"CID\": \"" +
                                 juce::String(expected.controllerCid) + "\""),
               juce::String(expected.name) +
                   " controller CID changed unexpectedly");
}

// JUCE's host stores the component's opaque bytes in a VST3PluginState
// envelope. Supply a linked Onda state to exercise the actual exported module,
// including setupProcessing, host preparation, and VST3's infinite-tail
// conversion.
[[nodiscard]] bool smokeLoadedProgram(juce::AudioPluginInstance &instance) {
  const auto source =
      juce::File{onda::plugin::pathToJuce(testTemporaryRoot() / "hosted.onda")};
  if (!source.replaceWithText(
          "outs { out1 }\n"
          "init { level = 0.25 }\n"
          "event note_on(id: i32, channel: i32, key: i32, velocity: f32) { level = velocity }\n"
          "event note_off(id: i32, channel: i32, key: i32, velocity: f32) { level = 0.0 }\n"
          "sample { out1 = level }\n"))
    return false;
  instance.setNonRealtime(true);
  setOndaProgram(instance, source);
  const auto channels =
      juce::jmax(ONDA_PLUGIN_INPUT_CHANNELS, ONDA_PLUGIN_OUTPUT_CHANNELS);
  for (int activation = 0; activation < 2; ++activation) {
    instance.prepareToPlay(48'000.0, 64);
    if (!check(std::isinf(instance.getTailLengthSeconds()),
               instance.getName() + " did not advertise VST3's infinite tail"))
      return false;
    juce::AudioBuffer<float> audio(channels, 64);
    audio.clear();
    juce::MidiBuffer midi;
    instance.processBlock(audio, midi);
    for (int frame = 0; frame < 64; ++frame) {
      if (!check(std::abs(audio.getSample(0, frame) - 0.25F) < 1.0e-6F,
                 instance.getName() +
                     " rendered fallback after host preparation"))
        return false;
    }
    instance.releaseResources();
  }
  // Exercise the exported VST3 Bypass parameter through JUCE's actual host.
  instance.prepareToPlay(48'000.0, 64);
  auto *const bypass = instance.getParameters()[32 + onda::plugin::pluginControlInputs];
  const auto dryChannels =
      instance.getName() == "OndaFX"
          ? juce::jmin(ONDA_PLUGIN_INPUT_CHANNELS, ONDA_PLUGIN_OUTPUT_CHANNELS)
          : 0;
  test::BypassReference reference;
  for (int step = 0; step < 3; ++step) {
    const auto bypassed = step == 1;
    bypass->setValueNotifyingHost(bypassed ? 1.0F : 0.0F);
    juce::AudioBuffer<float> audio(channels, 64);
    for (int channel = 0; channel < channels; ++channel)
      std::fill_n(audio.getWritePointer(channel), 64, 0.375F);
    juce::MidiBuffer midi;
    if (step == 0)
      midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), 0);
    else if (bypassed)
      midi.addEvent(juce::MidiMessage::noteOff(1, 60), 16);
    reference.begin(bypassed, 64);
    instance.processBlock(audio, midi);
    for (int channel = 0; channel < ONDA_PLUGIN_OUTPUT_CHANNELS; ++channel) {
      for (int frame = 0; frame < 64; ++frame) {
        const auto wet = channel == 0 &&
                                 (step == 0 || (step == 1 && frame < 16))
                             ? 1.0F
                             : 0.0F;
        const auto expected = reference.mix(
            wet, channel < dryChannels ? 0.375F : 0.0F, frame);
        if (!check(std::abs(audio.getSample(channel, frame) - expected) <
                       1.0e-6F,
                   instance.getName() + " failed host bypass or note release"))
          return false;
      }
    }
    reference.end(64);
  }
  instance.releaseResources();
  return true;
}

[[nodiscard]] bool smokeBypassTransitions(juce::AudioPluginInstance &instance) {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "vst3-bypass-transitions.onda")};
  if (!source.replaceWithText(test::bypassTransitionSource()))
    return false;
  setOndaProgram(instance, source, 0.5F, 3);
  instance.setNonRealtime(false);
  if constexpr (pluginControlInputs > 0) {
    auto layout = instance.getBusesLayout();
    layout.inputBuses.set(pluginMainInputBuses, juce::AudioChannelSet::mono());
    if (!instance.setBusesLayout(layout))
      return false;
  }
  const auto render = [&](juce::AudioBuffer<float> &audio,
                          juce::MidiBuffer &midi, const bool bypassed) {
    instance.getParameters()[32 + pluginControlInputs]
        ->setValueNotifyingHost(bypassed ? 1.0F : 0.0F);
    instance.processBlock(audio, midi);
    return true;
  };
  for (const auto sampleRate : {48'000.0, 96'000.0})
    if (!test::checkBypassTransitions(instance, instance.getName() == "OndaFX",
                                      sampleRate, render))
      return false;
  instance.getParameters()[32 + pluginControlInputs]->setValueNotifyingHost(0.0F);
  return instance.disableNonMainBuses();
}

[[nodiscard]] bool smokeControlInputs(juce::AudioPluginInstance &instance) {
  if constexpr (onda::plugin::pluginControlInputs > 0) {
    const juce::File source{onda::plugin::pathToJuce(
        testTemporaryRoot() / "vst3-control-inputs.onda")};
    if (!source.replaceWithText(test::controlInputSource()))
      return false;
    const std::array defaults{0.25F, 0.75F};
    setOndaProgram(instance, source, 0.5F, 3, defaults);
  }
  instance.setNonRealtime(false);
  const auto render = [&](juce::AudioBuffer<float> &audio,
                          juce::MidiBuffer &midi, const bool bypassed) {
    instance.getParameters()[32 + onda::plugin::pluginControlInputs]
        ->setValueNotifyingHost(bypassed ? 1.0F : 0.0F);
    instance.processBlock(audio, midi);
    return true;
  };
  if (!test::checkControlInputRouting(instance, instance.getName() == "OndaFX",
                                      render))
    return false;
  if constexpr (onda::plugin::pluginControlInputs > 0) {
    const juce::File all{onda::plugin::pathToJuce(
        testTemporaryRoot() / "vst3-all-control-inputs.onda")};
    if (!all.replaceWithText(test::allControlInputSource()))
      return false;
    const std::vector<float> defaults(
        static_cast<std::size_t>(onda::plugin::pluginControlInputs), 0.125F);
    setOndaProgram(instance, all, 0.5F, 3, defaults);
    if (!test::checkLastControlInputRouting(instance, render))
      return false;
  }
  const juce::File unused{onda::plugin::pathToJuce(
      testTemporaryRoot() / "vst3-unused-controls.onda")};
  if (!unused.replaceWithText(test::unusedControlInputSource()))
    return false;
  setOndaProgram(instance, unused, 0.5F, 3);
  return test::checkUnusedControlInputs(instance, render);
}

bool smokeSavedProject(juce::VST3PluginFormatHeadless &format,
                       const juce::PluginDescription &description,
                       juce::AudioPluginInstance &instance) {
  const juce::File root{onda::plugin::pathToJuce(testTemporaryRoot())};
  const auto project = root.getChildFile("saved-project-" + description.name);
  const auto source = project.getChildFile("main.onda");
  if (!project.createDirectory().wasOk() ||
      !project.getChildFile("dependency.onda")
           .replaceWithText("def saved_gain() { return 0.375 }\n") ||
      !source.replaceWithText(
          "import dependency\nouts { out1 }\n"
          "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "sample { out1 = saved_gain() }\n")) {
    std::cerr << "Could not write the saved-project fixture\n";
    return false;
  }
  const auto renders = [](juce::AudioPluginInstance &target) {
    target.setNonRealtime(true);
    target.prepareToPlay(48'000.0, 64);
    juce::AudioBuffer<float> audio(
        std::max(ONDA_PLUGIN_INPUT_CHANNELS, ONDA_PLUGIN_OUTPUT_CHANNELS), 64);
    audio.clear();
    juce::MidiBuffer midi;
    target.processBlock(audio, midi);
    const auto correct =
        std::all_of(audio.getReadPointer(0), audio.getReadPointer(0) + 64,
                    [](float sample) {
                      return test::withinTolerance(sample - 0.375F, 1.0e-6F);
                    });
    if (!correct)
      std::cerr << target.getName()
                << " saved-project output: " << audio.getSample(0, 0)
                << ", expected 0.375\n";
    target.releaseResources();
    return correct;
  };
  setOndaProgram(instance, source);
  if (!check(renders(instance),
             description.name + " could not render the project before saving"))
    return false;
  juce::MemoryBlock saved;
  instance.getStateInformation(saved);
  if (!check(!saved.isEmpty(),
             description.name + " returned an empty saved project") ||
      !check(project.deleteRecursively(),
             "Could not delete the saved project's original files"))
    return false;
  juce::String error;
  auto restored =
      format.createInstanceFromDescription(description, 48'000.0, 64, error);
  if (!restored) {
    std::cerr << error << '\n';
    return false;
  }
  restored->setStateInformation(saved.getData(),
                                static_cast<int>(saved.getSize()));
  return check(renders(*restored),
               description.name + " did not restore its saved program and "
                                  "imports without the original files");
}

[[nodiscard]] bool smokeProduct(juce::VST3PluginFormatHeadless &format,
                                const juce::File &module,
                                const ProductExpectation expected) {
  if (!check(module.existsAsFile(),
             "Expected a VST3 module binary: " + module.getFullPathName()))
    return false;
  const auto bundle = vst3BundleForModule(module);
  if (!check(bundle.isDirectory(),
             "VST3 bundle not found for " + module.getFullPathName()))
    return false;
  if (!checkMetadata(bundle, expected))
    return false;

  juce::OwnedArray<juce::PluginDescription> descriptions;
  format.findAllTypesForFile(descriptions, bundle.getFullPathName());
  if (!check(descriptions.size() == 1,
             "Expected one component in " + bundle.getFullPathName()))
    return false;

  const auto &description = *descriptions[0];
  if (!check(description.name == expected.name,
             "Unexpected product name: " + description.name))
    return false;
  if (!check(description.version == ONDA_PLUGIN_VERSION,
             description.name + " reported version " + description.version +
                 " instead of " ONDA_PLUGIN_VERSION)) {
    return false;
  }

  juce::String error;
  auto instance =
      format.createInstanceFromDescription(description, 48000.0, 512, error);
  if (!check(instance != nullptr,
             "Could not instantiate " + description.name + ": " + error))
    return false;
  auto secondInstance =
      format.createInstanceFromDescription(description, 48000.0, 512, error);
  if (!check(secondInstance != nullptr, "Could not create a second " +
                                            description.name +
                                            " instance: " + error)) {
    return false;
  }

  auto succeeded = true;
  const auto parameterCount = instance->getParameters().size();
  succeeded &= check(
      parameterCount == 2113 + onda::plugin::pluginControlInputs,
      description.name +
          " must expose 32 Onda slots, bypass, and 2080 VST3 MIDI mappings "
          "(found " +
          juce::String(parameterCount) + ")");
  for (int index = 0; index < juce::jmin(parameterCount, 32); ++index) {
    const auto slotNumber = index + 1;
    const auto expectedName = "Slot " + juce::String(index + 1);
    const auto sourceID = "slot" + juce::String(slotNumber).paddedLeft('0', 2);
    const auto expectedVstID = juce::String(
        juce::VST3ClientExtensions::convertJuceParameterId(sourceID, true));
    const auto *parameter = instance->getParameters()[index];
    succeeded &= check(parameter->getName(64) == expectedName,
                       description.name + " parameter name mismatch at index " +
                           juce::String(index));
    const auto *hosted =
        dynamic_cast<const juce::HostedAudioProcessorParameter *>(parameter);
    succeeded &=
        check(hosted != nullptr && hosted->getParameterID() == expectedVstID,
              description.name + " parameter ID mismatch at index " +
                  juce::String(index));
    succeeded &=
        check(parameter->isAutomatable(),
              description.name + " slot must be automatable at index " +
                  juce::String(index));
    succeeded &=
        check(std::abs(parameter->getValue() - 0.5F) < 0.000001F,
              description.name + " parameter default mismatch at index " +
                  juce::String(index));
  }
  if (parameterCount > 32) {
    const auto *bypass = instance->getParameters()[32 + onda::plugin::pluginControlInputs];
    succeeded &= check(bypass->getName(64) == "Bypass",
                       description.name + " final parameter must be Bypass");
    succeeded &= check(bypass->getNumSteps() == 2,
                       description.name + " bypass must be boolean");
    succeeded &= check(std::abs(bypass->getValue()) < 0.000001F,
                       description.name + " bypass must default off");
  }
  auto midiMappingsAreNonAutomatable = true;
  for (int index = 33 + onda::plugin::pluginControlInputs; index < parameterCount; ++index)
    midiMappingsAreNonAutomatable &=
        !instance->getParameters()[index]->isAutomatable();
  succeeded &= check(midiMappingsAreNonAutomatable,
                     description.name +
                         " VST3 MIDI mappings must remain non-automatable");

  succeeded &=
      check(instance->acceptsMidi(), description.name + " must accept MIDI");
  succeeded &= check(!instance->producesMidi(),
                     description.name + " must not produce MIDI");
  succeeded &= check(!instance->supportsDoublePrecisionProcessing(),
                     description.name + " must be f32-only");
  succeeded &=
      check(instance->getTotalNumInputChannels() == ONDA_PLUGIN_INPUT_CHANNELS,
            description.name + " input layout mismatch");
  succeeded &= check(instance->getTotalNumOutputChannels() ==
                         ONDA_PLUGIN_OUTPUT_CHANNELS,
                     description.name + " output layout mismatch");

  juce::MemoryBlock firstState;
  instance->getStateInformation(firstState);
  succeeded &=
      check(!firstState.isEmpty(), description.name + " returned empty state");
  instance->setStateInformation(firstState.getData(),
                                static_cast<int>(firstState.getSize()));
  juce::MemoryBlock secondState;
  instance->getStateInformation(secondState);
  succeeded &= check(firstState == secondState,
                     description.name + " state did not round-trip exactly");

  instance->prepareToPlay(48000.0, 512);
  secondInstance->prepareToPlay(48000.0, 512);
  const auto channels =
      juce::jmax(ONDA_PLUGIN_INPUT_CHANNELS, ONDA_PLUGIN_OUTPUT_CHANNELS);
  std::atomic<bool> concurrentProcessingSucceeded{true};
  const auto process = [&](juce::AudioPluginInstance &target) {
    juce::AudioBuffer<float> audio(channels, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), 16);
    for (int iteration = 0; iteration < 100; ++iteration) {
      for (int channel = 0; channel < channels; ++channel)
        std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(),
                    1.0F);
      target.processBlock(audio, midi);
      for (int channel = 0; channel < ONDA_PLUGIN_OUTPUT_CHANNELS; ++channel) {
        const auto expectedSample =
            expected.dryFallback && channel < ONDA_PLUGIN_INPUT_CHANNELS ? 1.0F
                                                                         : 0.0F;
        for (int frame = 0; frame < audio.getNumSamples(); ++frame) {
          if (!test::withinTolerance(audio.getSample(channel, frame) -
                                         expectedSample,
                                     0.000001F)) {
            concurrentProcessingSucceeded.store(false,
                                                std::memory_order_relaxed);
          }
        }
      }
    }
  };
  std::thread firstAudioThread(process, std::ref(*instance));
  std::thread secondAudioThread(process, std::ref(*secondInstance));
  firstAudioThread.join();
  secondAudioThread.join();
  succeeded &=
      check(concurrentProcessingSucceeded.load(std::memory_order_relaxed),
            description.name +
                " produced an unsafe fallback with two concurrent instances");
  instance->releaseResources();
  secondInstance->releaseResources();
  succeeded &= check(smokeLoadedProgram(*instance),
                     description.name + " smokeLoadedProgram failed");
  succeeded &= check(smokeSavedProject(format, description, *instance),
                     description.name + " saved-project restoration failed");
  succeeded &= check(smokeAutomation(*instance),
                     description.name + " smokeAutomation failed");
  succeeded &= check(smokeCallbackProcessing(*instance),
                     description.name + " smokeCallbackProcessing failed");
  succeeded &= check(smokeParameterDefaults(*instance),
                     description.name + " smokeParameterDefaults failed");
  succeeded &= check(smokeControlAutomation(*instance),
                     description.name + " smokeControlAutomation failed");
  succeeded &= check(smokeParameterConversions(format, description, module),
                     description.name + " smokeParameterConversions failed");
  succeeded &= check(smokeAutomationNotifications(format, description, module),
                     description.name + " smokeAutomationNotifications failed");
  succeeded &=
      check(smokeOfflineAutomationNotifications(format, description, module),
            description.name + " smokeOfflineAutomationNotifications failed");
  succeeded &= check(smokeControlInputs(*instance),
                     instance->getName() + " control input smoke failed");
  succeeded &= check(smokeBypassTransitions(*instance),
                     description.name + " smokeBypassTransitions failed");

  return succeeded;
}

} // namespace

int main(const int argc, const char *const *argv) {
  if (argc != 3) {
    std::cerr << "usage: onda_vst3_smoke <instrument-module> <effect-module>\n";
    return 2;
  }

  juce::ScopedJuceInitialiser_GUI juceInitialiser;
  if (!checkBundleLookup())
    return 1;
  juce::VST3PluginFormatHeadless format;
  constexpr std::array expectations{
      ProductExpectation{"OndaSynth", false, "ABCDEF019182FAEB4F6E64614F64796E",
                         "ABCDEF011234ABCD4F6E64614F64796E"},
      ProductExpectation{"OndaFX", true, "ABCDEF019182FAEB4F6E64614F656678",
                         "ABCDEF011234ABCD4F6E64614F656678"},
  };

  auto succeeded = true;
  for (std::size_t index = 0; index < expectations.size(); ++index) {
    succeeded &=
        smokeProduct(format, juce::File{argv[index + 1]}, expectations[index]);
  }
  return succeeded ? 0 : 1;
}
