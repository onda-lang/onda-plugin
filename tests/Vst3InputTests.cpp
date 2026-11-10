#include "Vst3InputTests.h"

#include "JucePath.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"
#include "TestWait.h"
#include "Vst3ControllerTestSupport.h"
#include "Vst3RealtimeTestSupport.h"
#include "Vst3TestSupport.h"

#include <algorithm>
#include <array>
#include <iostream>

namespace {
using namespace Steinberg;

class InputChanges final
    : public vst3_test::HostInterface<Vst::IParameterChanges>,
      public vst3_test::HostInterface<Vst::IParamValueQueue> {
public:
  Vst::ParamID id{};
  int points{1};
  double value{};
  bool ramp{};

  int32 PLUGIN_API getParameterCount() override { return 1; }
  Vst::IParamValueQueue *PLUGIN_API getParameterData(int32 index) override {
    return index == 0 ? this : nullptr;
  }
  Vst::IParamValueQueue *PLUGIN_API addParameterData(const Vst::ParamID &,
                                                     int32 &) override {
    return nullptr;
  }
  Vst::ParamID PLUGIN_API getParameterId() override { return id; }
  int32 PLUGIN_API getPointCount() override { return points; }
  tresult PLUGIN_API getPoint(int32 index, int32 &offset,
                              double &output) override {
    if (index < 0 || index >= points)
      return kInvalidArgument;
    offset = index * 512 / points;
    output = ramp ? static_cast<double>(index) / (points - 1) : value;
    return kResultOk;
  }
  tresult PLUGIN_API addPoint(int32, double, int32 &) override {
    return kNotImplemented;
  }
};

class NoteEvents final : public vst3_test::HostInterface<Vst::IEventList> {
public:
  int count{};
  bool release{true};
  bool sysex{};

  int32 PLUGIN_API getEventCount() override { return count; }
  tresult PLUGIN_API getEvent(int32 index, Vst::Event &event) override {
    if (index < 0 || index >= count)
      return kInvalidArgument;
    event = {};
    event.sampleOffset = index * 512 / count;
    if (sysex) {
      event.type = Vst::Event::kDataEvent;
      event.data = {static_cast<uint32>(sysexBytes.size()),
                    Vst::DataEvent::kMidiSysEx, sysexBytes.data()};
      return kResultOk;
    }
    const auto note = release ? index % (count / 2) : index;
    const auto channel = static_cast<int16>((note / 128) % 16);
    const auto pitch = static_cast<int16>(note % 128);
    if (!release || index < count / 2) {
      event.type = Vst::Event::kNoteOnEvent;
      event.noteOn = {channel, pitch, 0.0F, 0.75F, 0, -1};
    } else {
      event.type = Vst::Event::kNoteOffEvent;
      event.noteOff = {channel, pitch, 0.0F, -1, 0.0F};
    }
    return kResultOk;
  }
  tresult PLUGIN_API addEvent(Vst::Event &) override { return kNotImplemented; }

private:
  inline static const auto sysexBytes = [] {
    std::array<uint8, 4096> bytes{};
    bytes.front() = 0xf0;
    bytes.back() = 0xf7;
    return bytes;
  }();
};

Vst::ParamID parameterId(juce::AudioPluginInstance &instance, const int index) {
  const auto *parameter = dynamic_cast<juce::HostedAudioProcessorParameter *>(
      instance.getParameters()[index]);
  return static_cast<Vst::ParamID>(
      parameter->getParameterID().getLargeIntValue());
}

class InputHost {
public:
  explicit InputHost(juce::AudioPluginInstance &instance)
      : audio(std::max(ONDA_PLUGIN_INPUT_CHANNELS, ONDA_PLUGIN_OUTPUT_CHANNELS),
              512) {
    instance.getVST3Client()->getIComponentPtr()->queryInterface(
        Vst::IAudioProcessor::iid, reinterpret_cast<void **>(&processor));
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
      pointers[static_cast<std::size_t>(channel)] =
          audio.getWritePointer(channel);
    if constexpr (ONDA_PLUGIN_INPUT_CHANNELS > 0) {
      inputs[0].numChannels = ONDA_PLUGIN_INPUT_CHANNELS;
      inputs[0].channelBuffers32 = pointers.data();
    }
    output.numChannels = ONDA_PLUGIN_OUTPUT_CHANNELS;
    output.channelBuffers32 = pointers.data();
    data.processMode = Vst::kRealtime;
    data.symbolicSampleSize = Vst::kSample32;
    data.numSamples = 512;
    data.numInputs = onda::plugin::pluginInputBuses;
    data.numOutputs = 1;
    data.inputs = inputs.data();
    data.outputs = &output;
  }
  ~InputHost() {
    if (processor != nullptr)
      processor->release();
  }

  bool process() {
    // The exported processor writes through retained pointers, bypassing
    // AudioBuffer's clear flag. Clear the actual samples on every callback.
    audio.setNotClear();
    audio.clear();
    return processor != nullptr &&
           vst3_test::processRealtime(*processor, data) == kResultOk;
  }
  bool constant(const float expected) const {
    for (int frame = 0; frame < 512; ++frame)
      if (!test::withinTolerance(audio.getSample(0, frame) - expected, 1.0e-6F))
        return false;
    return true;
  }

  juce::AudioBuffer<float> audio;
  Vst::ProcessData data{};

private:
  std::array<float *,
             std::max(ONDA_PLUGIN_INPUT_CHANNELS, ONDA_PLUGIN_OUTPUT_CHANNELS)>
      pointers{};
  std::array<Vst::AudioBusBuffers, onda::plugin::pluginInputBuses> inputs{};
  Vst::AudioBusBuffers output{};
  Vst::IAudioProcessor *processor{};
};

bool midiInput(juce::AudioPluginInstance &instance, const bool controllers) {
  InputHost host(instance);
  NoteEvents notes;
  InputChanges changes;
  changes.id =
      parameterId(instance, ONDA_PLUGIN_PARAMETER_SLOTS +
                                onda::plugin::pluginControlInputs + 1 + 74);
  changes.ramp = true;
  notes.count = controllers ? 0 : 384;
  changes.points = controllers ? 512 : 1;
  host.data.inputEvents = &notes;
  host.data.inputParameterChanges = controllers ? &changes : nullptr;
  if (!host.process())
    return false;
  float control{};
  const auto checkDense = [&] {
    int held{};
    int event{};
    int point{};
    for (int frame = 0; frame < 512; ++frame) {
      while (host.data.inputParameterChanges != nullptr &&
             point < changes.points && point * 512 / changes.points == frame) {
        control = static_cast<float>(std::min(
                      127, static_cast<int>(static_cast<double>(point) /
                                            (changes.points - 1) * 128.0))) /
                  127.0F;
        ++point;
      }
      while (event < notes.count && event * 512 / notes.count == frame) {
        held += event < notes.count / 2 ? 1 : -1;
        ++event;
      }
      if (!test::withinTolerance(host.audio.getSample(0, frame) -
                                     (static_cast<float>(held) + control),
                                 1.0e-6F))
        return false;
    }
    return true;
  };
  if (!checkDense())
    return false;
  notes.count = controllers ? 0 : 4096;
  changes.points = 4096;
  if (!host.process() || !checkDense())
    return false;
  notes.count = 384;
  changes.points = 512;
  host.data.inputParameterChanges = &changes;
  if (!host.process() || !checkDense())
    return false;

  // Unsupported variable-size events must be filtered before JUCE allocates
  // an owning MidiMessage. A held note must survive the ignored event.
  host.data.inputParameterChanges = nullptr;
  notes = NoteEvents{};
  notes.count = 2;
  notes.release = false;
  if (!host.process())
    return false;
  notes.sysex = true;
  notes.count = 1;
  if (!host.process() || !host.constant(3.0F))
    return false;

  // A whole overflowing batch is discarded and held notes are released.
  // Exercise each source and their shared budget, then verify recovery.
  for (const auto mode : {0, 1, 2, 3}) {
    notes.sysex = false;
    notes.release = true;
    notes.count = mode == 0 ? 4098 : mode == 2 ? 2048 : 0;
    changes.points = mode == 1 || mode == 3 ? 4097 : 3000;
    host.data.inputParameterChanges = mode == 0 ? nullptr : &changes;
    if (mode == 3) {
      host.data.numSamples = host.data.numInputs = host.data.numOutputs = 0;
      if (!host.process())
        return false;
      host.data.numSamples = 512;
      host.data.numInputs = onda::plugin::pluginInputBuses;
      host.data.numOutputs = 1;
      host.data.inputParameterChanges = nullptr;
    }
    if (!host.process() || !host.constant(1.0F))
      return false;
    notes.count = 2;
    notes.release = false;
    host.data.inputParameterChanges = nullptr;
    if (!host.process())
      return false;
  }
  return true;
}

bool bypassInput(juce::AudioPluginInstance &instance,
                 const juce::File &module) {
  InputHost host(instance);
  vst3_test::ObservedController controller;
  if (!controller.connect(*instance.getVST3Client()->getIComponentPtr(),
                          module)) {
    std::cerr << "Could not connect the bypass observer: "
              << module.getFullPathName() << '\n';
    return false;
  }
  InputChanges bypass;
  bypass.id = parameterId(instance, ONDA_PLUGIN_PARAMETER_SLOTS +
                                        onda::plugin::pluginControlInputs);
  // Hold two notes before bypass; DSP must continue advancing while inaudible.
  NoteEvents notes;
  notes.count = 2;
  notes.release = false;
  host.data.inputEvents = &notes;
  if (!host.process())
    return false;
  host.data.inputEvents = nullptr;
  host.data.inputParameterChanges = &bypass;
  for (const auto value : {1.0, 0.0, 1.0}) {
    bypass.value = value;
    if (!host.process())
      return false;
    // At 48 kHz the five millisecond transition settles within this callback.
    const auto expected = value >= 0.5 ? 0.0F : 2.0F;
    if (!test::withinTolerance(host.audio.getSample(0, 511) - expected,
                               1.0e-6F)) {
      std::cerr << "Bypass " << value << " rendered "
                << host.audio.getSample(0, 511) << "; expected " << expected
                << '\n';
      return false;
    }
    if (!test::waitUntil(
            [&] {
              return test::withinTolerance(controller.value(bypass.id) - value,
                                           1.0e-6);
            },
            [] {
              juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
            }) ||
        controller.edits != 0 || controller.gestures != 0) {
      std::cerr << "Bypass controller: " << controller.value(bypass.id)
                << "; expected " << value << ", edits " << controller.edits
                << ", gestures " << controller.gestures << '\n';
      return false;
    }
  }
  return true;
}
} // namespace

bool smokeFreshBypassCallbacks(juce::AudioPluginInstance &instance,
                               Steinberg::Vst::IAudioProcessor &processor,
                               Steinberg::Vst::ProcessData &data) {
  InputChanges bypass;
  bypass.id = parameterId(instance, ONDA_PLUGIN_PARAMETER_SLOTS +
                                        onda::plugin::pluginControlInputs);
  data.inputParameterChanges = &bypass;
  auto passed = true;
  for (const auto value : {1.0, 0.0}) {
    bypass.value = value;
    juce::FloatVectorOperations::disableDenormalisedNumberSupport(false);
    passed &=
        vst3_test::processRealtime(processor, data) == Steinberg::kResultOk &&
        juce::FloatVectorOperations::areDenormalsDisabled();
  }
  data.inputParameterChanges = nullptr;
  return passed;
}

bool smokeVst3MidiInput(juce::VST3PluginFormatHeadless &format,
                        const juce::PluginDescription &description,
                        const juce::File &module) {
  const juce::File source{
      onda::plugin::pathToJuce(testTemporaryRoot() / "vst3-midi-input.onda")};
  if (!source.replaceWithText(R"(
init { held: i32 = 0; control = 0.0 }
event note_on(id: i32, channel: i32, key: i32, velocity: f32) { held += 1 }
event note_off(id: i32, channel: i32, key: i32, velocity: f32) { held -= 1 }
event cc(channel: i32, index: i32, value: f32) { control = value }
outs { out }
sample { out = f32(held) + control }
)"))
    return false;
  for (const auto controllers : {false, true}) {
    juce::String error;
    auto instance =
        format.createInstanceFromDescription(description, 48000.0, 512, error);
    if (!instance || !instance->disableNonMainBuses())
      return false;
    setOndaProgram(*instance, source);
    instance->prepareToPlay(48000.0, 512);
    if (!midiInput(*instance, controllers)) {
      std::cerr << description.name << " failed "
                << (controllers ? "controller" : "note") << " input audit\n";
      return false;
    }
    instance->releaseResources();
  }
  juce::String error;
  auto instance =
      format.createInstanceFromDescription(description, 48000.0, 512, error);
  if (!instance || !instance->disableNonMainBuses())
    return false;
  setOndaProgram(*instance, source);
  instance->prepareToPlay(48000.0, 512);
  if (!bypassInput(*instance, module)) {
    std::cerr << description.name << " failed bypass delivery\n";
    return false;
  }
  instance->releaseResources();
  return true;
}
