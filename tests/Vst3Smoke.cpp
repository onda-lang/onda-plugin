#include "JucePath.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

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

[[nodiscard]] juce::File bundleForModule(const juce::File &module) {
  auto candidate = module;
  for (;;) {
    if (candidate.isDirectory() && candidate.hasFileExtension("vst3"))
      return candidate;
    const auto parent = candidate.getParentDirectory();
    if (parent == candidate)
      return {};
    candidate = parent;
  }
}

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
  return check(bundleForModule(module) == bundle &&
                   bundleForModule(bundle) == bundle &&
                   bundleForModule(root.getChildFile("missing.bin")) ==
                       juce::File{},
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
          "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "sample { out1 = 0.25 }\n"))
    return false;
  juce::MemoryBlock componentState;
  {
    juce::MemoryOutputStream stream(componentState, false);
    stream.writeInt(0x41444e4f);
    stream.writeInt(4);
    stream.writeString(source.getFullPathName());
    stream.writeString({});
    stream.writeInt(0);
    stream.writeInt64(0);
    for (int slot = 0; slot < 32; ++slot)
      stream.writeFloat(0.5F);
    stream.writeInt(480);
    stream.writeInt(720);
    stream.writeBool(false);
    stream.writeString({});
  }
  juce::XmlElement state("VST3PluginState");
  state.createNewChildElement("IComponent")
      ->addTextElement(componentState.toBase64Encoding());
  juce::MemoryBlock hostState;
  juce::AudioProcessor::copyXmlToBinary(state, hostState);
  instance.setNonRealtime(true);
  instance.setStateInformation(hostState.getData(),
                               static_cast<int>(hostState.getSize()));
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
  return true;
}

[[nodiscard]] bool smokeProduct(juce::VST3PluginFormatHeadless &format,
                                const juce::File &module,
                                const ProductExpectation expected) {
  const auto bundle = bundleForModule(module);
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
      parameterCount == 2113,
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
    const auto *bypass = instance->getParameters()[32];
    succeeded &= check(bypass->getName(64) == "Bypass",
                       description.name + " final parameter must be Bypass");
    succeeded &= check(bypass->getNumSteps() == 2,
                       description.name + " bypass must be boolean");
    succeeded &= check(std::abs(bypass->getValue()) < 0.000001F,
                       description.name + " bypass must default off");
  }
  auto midiMappingsAreNonAutomatable = true;
  for (int index = 33; index < parameterCount; ++index)
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
  succeeded &= smokeLoadedProgram(*instance);

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
