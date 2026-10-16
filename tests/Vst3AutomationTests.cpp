#include "Vst3AutomationTests.h"

#include "CallbackProcessingTestSupport.h"
#include "ControlInputTestSupport.h"
#include "JucePath.h"
#include "Product.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"
#include "TestWait.h"
#include "Vst3RealtimeAudit.h"
#include "Vst3TestSupport.h"
#include "Vst3ControllerTestSupport.h"

#define JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY 1
#include <juce_audio_processors_headless/format_types/juce_VST3Headers.h>

#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <thread>

#if defined(__linux__)
#include <cstdlib>
#include <dlfcn.h>
#endif

namespace {
using namespace Steinberg;
using vst3_test::HostInterface;
using vst3_test::ObservedController;
using vst3_test::ReleaseInterface;

tresult processRealtime(Vst::IAudioProcessor &processor,
                        Vst::ProcessData &data) {
#if defined(__linux__)
  static const auto begin = reinterpret_cast<decltype(&onda_vst3_audit_begin)>(
      dlsym(RTLD_DEFAULT, "onda_vst3_audit_begin"));
  static const auto end = reinterpret_cast<decltype(&onda_vst3_audit_end)>(
      dlsym(RTLD_DEFAULT, "onda_vst3_audit_end"));
  if (begin != nullptr && end != nullptr) {
    begin();
    const auto result = processor.process(data);
    Vst3RealtimeCounts counts;
    end(&counts);
    // JUCE retains one callback mutex. Managed automation must add no locks,
    // allocations or frees, including queue flushes and MIDI segment splits.
    if (counts.hasViolations(1)) {
      std::cerr << "VST3 realtime audit: " << counts.locks << " locks, "
                << counts.allocations << " allocations, " << counts.frees
                << " frees\n";
      return kResultFalse;
    }
    return result;
  }
  if (std::getenv("ONDA_REQUIRE_VST3_AUDIT") != nullptr) {
    std::cerr << "VST3 realtime audit library was not loaded\n";
    return kResultFalse;
  }
#endif
  return processor.process(data);
}

struct Point {
  int32 offset;
  double value;
};

class ValueQueue final : public HostInterface<Vst::IParamValueQueue> {
public:
  ValueQueue(const char *id, const std::span<const Point> points)
      : id_(juce::VST3ClientExtensions::convertJuceParameterId(id)),
        points_(points) {}
  Vst::ParamID PLUGIN_API getParameterId() override { return id_; }
  int32 PLUGIN_API getPointCount() override {
    return static_cast<int32>(points_.size());
  }
  tresult PLUGIN_API getPoint(const int32 index, int32 &offset,
                              double &value) override {
    if (index < 0 || static_cast<std::size_t>(index) >= points_.size())
      return kInvalidArgument;
    const auto &point = points_[static_cast<std::size_t>(index)];
    offset = point.offset;
    value = point.value;
    return kResultOk;
  }
  tresult PLUGIN_API addPoint(int32, double, int32 &) override {
    return kNotImplemented;
  }

private:
  Vst::ParamID id_;
  std::span<const Point> points_;
};

class Changes final : public HostInterface<Vst::IParameterChanges> {
public:
  explicit Changes(const std::span<ValueQueue *const> queues)
      : queues_(queues) {}
  int32 PLUGIN_API getParameterCount() override {
    return static_cast<int32>(queues_.size());
  }
  Vst::IParamValueQueue *PLUGIN_API
  getParameterData(const int32 index) override {
    return index >= 0 && static_cast<std::size_t>(index) < queues_.size()
               ? queues_[static_cast<std::size_t>(index)]
               : nullptr;
  }
  Vst::IParamValueQueue *PLUGIN_API addParameterData(const Vst::ParamID &,
                                                     int32 &) override {
    return nullptr;
  }

private:
  std::span<ValueQueue *const> queues_;
};

// Count unsolicited output changes without allocating in the process call.
class OutputChanges final : public HostInterface<Vst::IParameterChanges> {
public:
  int32 PLUGIN_API getParameterCount() override { return count; }
  Vst::IParamValueQueue *PLUGIN_API getParameterData(int32) override {
    return nullptr;
  }
  Vst::IParamValueQueue *PLUGIN_API addParameterData(const Vst::ParamID &,
                                                     int32 &index) override {
    index = count++;
    return nullptr;
  }
  int count{};
};

class Events final : public HostInterface<Vst::IEventList> {
public:
  Vst::Event note{};
  int32 PLUGIN_API getEventCount() override { return 1; }
  tresult PLUGIN_API getEvent(const int32 index, Vst::Event &event) override {
    if (index != 0)
      return kInvalidArgument;
    event = note;
    return kResultOk;
  }
  tresult PLUGIN_API addEvent(Vst::Event &) override { return kNotImplemented; }
};

} // namespace

bool smokeParameterConversions(juce::VST3PluginFormatHeadless &format,
                               const juce::PluginDescription &description,
                               const juce::File &module) {
  using namespace onda::plugin;
  ObservedController controller;
  juce::String error;
  auto instance =
      format.createInstanceFromDescription(description, 48'000.0, 8, error);
  if (instance == nullptr)
    return false;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "parameter-conversions.onda")};
  auto program = juce::String{};
  if constexpr (pluginControlInputs > 0) {
    program = "ins {\n" + test::mainAudioInputDeclaration() +
              "cv = 0.0 {-2.0, 2.0}\n";
    if constexpr (pluginControlInputs > 1)
      program += "fixed = 3.0 {3.0, 3.0}\n";
    program += "}\n";
  }
  program +=
      "params {\n"
      "frequency: f64 = 1.0 {1.0, 100.0, log}\n"
      "level = 0.0 {-2.0, 2.0}\n"
      "mode: i32 = 0 {0, 4, step = 1}\n"
      "gate: bool = false\n"
      "below: i32 = 0 {0, 2147483645}\n"
      "edge: i32 = 0 {0, 2147483646}\n"
      "maximum: i32 = 0 {0, 2147483647}\n"
      "full: i32 = 0 {-2147483648, 2147483647}\n"
      "wide: i64 = 0 {-2147483648, 2147483647}\n"
      "}\nouts { out }\n"
      "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
      "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
      "sample { out = f32(frequency) }\n";
  if (!source.replaceWithText(program) || !instance->disableNonMainBuses())
    return false;
  setOndaProgram(*instance, source);
  instance->prepareToPlay(48'000.0, 8);
  if (!controller.connect(*instance->getVST3Client()->getIComponentPtr(),
                          module))
    return false;
  if (!test::waitUntil(
          [&] {
            return test::withinTolerance(controller.plain("slot01", 0.5) - 10.0,
                                         1.0e-12);
          },
          [] {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
          })) {
    std::cerr << "VST3 controller did not expose the logarithmic domain\n";
    return false;
  }
  const auto check = [&](const char *id, const double normalized,
                         const double plain) {
    const auto rendered = controller.plain(id, normalized);
    const auto recovered = controller.normalized(id, plain);
    if (!test::withinTolerance(rendered - plain, 1.0e-12) ||
        !test::withinTolerance(recovered - normalized, 1.0e-12)) {
      std::cerr << "VST3 conversion " << id << ": " << normalized << " -> "
                << rendered << ", " << plain << " -> " << recovered << '\n';
      return false;
    }
    return true;
  };
  const auto checkSlot = [&](const int slot, const double normalized,
                             const double plain) {
    if (slot > ONDA_PLUGIN_PARAMETER_SLOTS)
      return true;
    const auto id = "slot" + juce::String(slot).paddedLeft('0', 2);
    return check(id.toRawUTF8(), normalized, plain);
  };
  if (!checkSlot(1, 0.0, 1.0) || !checkSlot(1, 0.5, 10.0) ||
      !checkSlot(1, 1.0, 100.0) || !checkSlot(2, 0.5, 0.0) ||
      !checkSlot(2, 1.0, 2.0) || !checkSlot(3, 0.5, 2.0) ||
      !checkSlot(4, 0.0, 0.0) || !checkSlot(4, 1.0, 1.0))
    return false;
  if constexpr (ONDA_PLUGIN_PARAMETER_SLOTS > 9) {
    if (!checkSlot(ONDA_PLUGIN_PARAMETER_SLOTS, 0.25, 0.25))
      return false;
  }
  if constexpr (pluginControlInputs > 0)
    if (!check("input01", 0.0, -2.0) || !check("input01", 0.5, 0.0) ||
        !check("input01", 1.0, 2.0))
      return false;
  if constexpr (pluginControlInputs > 1)
    if (!check("input02", 0.0, 3.0) ||
        !std::is_eq(controller.plain("input02", 1.0) <=> 3.0))
      return false;
  // Inspect native VST3 metadata: JUCE's host also uses INT_MAX as a
  // continuous sentinel and cannot faithfully expose these large counts.
  constexpr std::array counts{
      std::pair{1, int32{0}},          std::pair{3, int32{4}},
      std::pair{4, int32{1}},          std::pair{5, int32{2147483645}},
      std::pair{6, int32{2147483646}}, std::pair{7, int32{2147483647}},
      std::pair{8, int32{2147483647}}, std::pair{9, int32{2147483647}}};
  for (const auto &[slot, steps] : counts) {
    const auto id = "slot" + juce::String(slot).paddedLeft('0', 2);
    const auto expected = slot <= ONDA_PLUGIN_PARAMETER_SLOTS ? steps : -1;
    if (controller.stepCount(id.toRawUTF8()) != expected) {
      std::cerr << "VST3 step count " << id << ": actual "
                << controller.stepCount(id.toRawUTF8()) << ", expected "
                << expected << '\n';
      return false;
    }
  }
  if constexpr (ONDA_PLUGIN_PARAMETER_SLOTS > 9) {
    const auto id =
        "slot" + juce::String(ONDA_PLUGIN_PARAMETER_SLOTS).paddedLeft('0', 2);
    if (controller.stepCount(id.toRawUTF8()) != 0)
      return false;
  }
  instance->releaseResources();
  return true;
}

bool smokeAutomationNotifications(juce::VST3PluginFormatHeadless &format,
                                  const juce::PluginDescription &description,
                                  const juce::File &module) {
  using namespace onda::plugin;
  // Controllers own listeners on the component's parameters. Keep the
  // observer alive until the host has disconnected and destroyed its instance.
  ObservedController controller;
  juce::String error;
  auto ownedInstance =
      format.createInstanceFromDescription(description, 48'000.0, 8, error);
  if (ownedInstance == nullptr) {
    std::cerr << error << '\n';
    return false;
  }
  auto &instance = *ownedInstance;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "automation-notifications.onda")};
  juce::String program;
  if constexpr (pluginInputChannels > 0 || pluginControlInputs > 0) {
    program = "ins {\n" + test::mainAudioInputDeclaration();
    if constexpr (pluginControlInputs > 0)
      program += "modulation = 0.25 {0.0, 1.0}\n";
    program += "}\n";
  }
  program +=
      "params { precise: f64 = 0.0 {0.0, 1.0} }\nouts { out1 }\n"
      "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
      "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
      "sample { out1 = f32((precise - 0.5) * 1099511627776.0)";
  if constexpr (pluginControlInputs > 0)
    program += " + modulation";
  program += " }\n";
  if (!source.replaceWithText(program) || !instance.disableNonMainBuses())
    return false;
  setOndaProgram(instance, source, 0.0F, 1);
  instance.setNonRealtime(false);
  instance.prepareToPlay(48'000.0, 8);
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  audio.clear();
  juce::MidiBuffer midi;
  // Consume any pending host state updates before submitting exact queues.
  instance.processBlock(audio, midi);
  auto *client = instance.getVST3Client();
  if (client == nullptr)
    return false;
  auto &component = *client->getIComponentPtr();
  if (!controller.connect(component, module))
    return false;
  const auto valueRefreshes = controller.valueRefreshes;
  Vst::IAudioProcessor *raw{};
  if (component.queryInterface(Vst::IAudioProcessor::iid,
                               reinterpret_cast<void **>(&raw)) != kResultOk)
    return false;
  std::unique_ptr<Vst::IAudioProcessor, ReleaseInterface> processor{raw};
  std::array<float *, 64> pointers{};
  for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    pointers[static_cast<std::size_t>(channel)] =
        audio.getWritePointer(channel);
  std::array<Vst::AudioBusBuffers, pluginInputBuses> inputs{};
  if constexpr (pluginInputChannels > 0) {
    inputs[0].numChannels = pluginInputChannels;
    inputs[0].channelBuffers32 = pointers.data();
  }
  Vst::AudioBusBuffers output{};
  output.numChannels = pluginOutputChannels;
  output.channelBuffers32 = pointers.data();
  constexpr auto exact = 0.5 + 0x1p-40;
  const std::array points{Point{0, exact}, Point{7, exact}};
  const std::array controlPoints{Point{0, 0.75}};
  ValueQueue precise{"slot01", points}, control{"input01", controlPoints};
  const std::array queues{&precise, &control};
  Changes changes{std::span<ValueQueue *const>{queues}.first(
      pluginControlInputs > 0 ? 2U : 1U)};
  Vst::ProcessData data{};
  data.processMode = Vst::kRealtime;
  data.symbolicSampleSize = Vst::kSample32;
  data.numSamples = 8;
  data.numInputs = pluginInputBuses;
  data.numOutputs = 1;
  data.inputs = inputs.data();
  data.outputs = &output;
  data.inputParameterChanges = &changes;
  const auto correctOutput = [&] {
    const auto expected = pluginControlInputs > 0 ? 1.75F : 1.0F;
    return std::all_of(audio.getReadPointer(0), audio.getReadPointer(0) + 8,
                       [expected](const float sample) {
                         return test::withinTolerance(sample - expected,
                                                      1.0e-6F);
                       });
  };
  if (processRealtime(*processor, data) != kResultOk || !correctOutput())
    return false;
  // Wait for an observable controller update, proving that the real timer
  // delivered the notifications rather than merely waiting for no edits.
  if (!test::waitUntil(
          [&] {
            return std::is_eq(controller.value("slot01") <=> 0.5) &&
                   (pluginControlInputs == 0 ||
                    std::is_eq(controller.value("input01") <=> 0.75));
          },
          [] {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
          }))
    return false;
  if (controller.edits != 0 || controller.gestures != 0 ||
      controller.valueRefreshes != valueRefreshes) {
    std::cerr << instance.getName()
              << " echoed host automation through controller edits/refreshes\n";
    return false;
  }
  // JUCE's original host must not receive rounded feedback that overwrites
  // the precise DSP endpoint on the following callback.
  for (int callback = 0; callback < 2; ++callback) {
    instance.processBlock(audio, midi);
    if (!correctOutput()) {
      std::cerr << instance.getName()
                << " lost the exact endpoint after controller notification\n";
      return false;
    }
  }
  instance.releaseResources();
  return true;
}

bool smokeParameterDefaults(juce::AudioPluginInstance &instance) {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "vst3-parameter-default.onda")};
  ScalarValue floating{0.5000000005};
  floating.scalarDefault = ScalarValue::ScalarDefault{
      1.0, {-1.0e9, 1.0e9, 0.0, 0.0, 0U, ScalarDomain::Type::f64}};
  ScalarValue integer{1.0};
  ScalarDomain integerDomain{0.0, 1.0, 1.0, 0.0, 1U, ScalarDomain::Type::i64};
  integerDomain.defaultMayExceedRange = true;
  integer.scalarDefault =
      ScalarValue::ScalarDefault{std::int64_t{9007199254740993}, integerDomain};
  struct Case {
    const char *declaration;
    const char *dsp;
    ScalarValue value;
    double endpoint;
    float defaultOutput;
    float automatedOutput;
  };
  const Case cases[]{
      {"p: f64 = 1.0 {-1000000000.0, 1000000000.0}",
       "sample { out = f32(p) }\n", floating, 0.5, 1.0F, 0.0F},
      {"p: i64 = 9007199254740993; following = 0.25 {0.0, 1.0}",
       "init { pinned = p }\n"
       "sample { if p == 9007199254740993 && pinned == 9007199254740993 "
       "{ out = following } else { out = f32(p) + 2.0 } }\n",
       integer, 1.0, 0.25F, 3.0F},
  };
  for (const auto &testCase : cases) {
    if (!source.replaceWithText(
            "params { " + juce::String{testCase.declaration} +
            " }\n"
            "outs { out }\n"
            "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
            "event note_off(id: i32, channel: i32, key: i32, velocity: f32) "
            "{}\n" +
            testCase.dsp) ||
        !instance.disableNonMainBuses())
      return false;
    const std::array parameters{testCase.value, ScalarValue{0.25}};
    setOndaProgram(instance, source, 0.5F, 1, {}, parameters);
    instance.setNonRealtime(false);
    instance.prepareToPlay(48'000.0, 8);
    juce::AudioBuffer<float> audio(
        std::max(pluginInputChannels, pluginOutputChannels), 8);
    juce::MidiBuffer midi;
    instance.processBlock(audio, midi);
    if (!std::is_eq(audio.getSample(0, 0) <=> testCase.defaultOutput))
      return false;
    Vst::IAudioProcessor *raw{};
    if (instance.getVST3Client()->getIComponentPtr()->queryInterface(
            Vst::IAudioProcessor::iid, reinterpret_cast<void **>(&raw)) !=
        kResultOk)
      return false;
    std::unique_ptr<Vst::IAudioProcessor, ReleaseInterface> processor{raw};
    std::array<float *, 64> pointers{};
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
      pointers[static_cast<std::size_t>(channel)] =
          audio.getWritePointer(channel);
    std::array<Vst::AudioBusBuffers, pluginInputBuses> inputs{};
    if constexpr (pluginInputChannels > 0) {
      inputs[0].numChannels = pluginInputChannels;
      inputs[0].channelBuffers32 = pointers.data();
    }
    Vst::AudioBusBuffers output{};
    output.numChannels = pluginOutputChannels;
    output.channelBuffers32 = pointers.data();
    const std::array points{Point{0, testCase.endpoint}};
    ValueQueue queue{"slot01", points};
    const std::array queues{&queue};
    Changes changes{queues};
    Vst::ProcessData data{};
    data.processMode = Vst::kRealtime;
    data.symbolicSampleSize = Vst::kSample32;
    data.numSamples = 8;
    data.numInputs = pluginInputBuses;
    data.numOutputs = 1;
    data.inputs = inputs.data();
    data.outputs = &output;
    data.inputParameterChanges = &changes;
    // The host endpoint repeats the JUCE float, yet must replace the exact
    // restored default in both this callback and the following hosted callback.
    if (processRealtime(*processor, data) != kResultOk ||
        !std::is_eq(audio.getSample(0, 0) <=> testCase.automatedOutput))
      return false;
    instance.processBlock(audio, midi);
    const auto correct =
        std::is_eq(audio.getSample(0, 0) <=> testCase.automatedOutput);
    instance.releaseResources();
    if (!correct)
      return false;
  }
  return true;
}

bool smokeOfflineAutomationNotifications(
    juce::VST3PluginFormatHeadless &format,
    const juce::PluginDescription &description, const juce::File &module) {
  using namespace onda::plugin;
  ObservedController controller;
  juce::String error;
  auto instance =
      format.createInstanceFromDescription(description, 48'000.0, 8, error);
  if (instance == nullptr) {
    std::cerr << error << '\n';
    return false;
  }
  const juce::File source{pathToJuce(testTemporaryRoot() /
                                     "offline-automation-notifications.onda")};
  auto program = juce::String{};
  if constexpr (pluginControlInputs > 0)
    program = "ins {\n" + test::mainAudioInputDeclaration() +
              "modulation = 0.25 {0.0, 1.0}\n}\n";
  program +=
      "params { precise: f64 = 0.5 {0.0, 1.0} }\nouts { out1 }\n"
      "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
      "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
      "sample { out1 = f32((precise - f64(f32(precise))) * 1099511627776.0)";
  if constexpr (pluginControlInputs > 0)
    program += " + modulation";
  program += " }\n";
  if (!source.replaceWithText(program) || !instance->disableNonMainBuses())
    return false;
  const std::array initialControls{0.25F};
  setOndaProgram(*instance, source, 0.5F, 1, initialControls);
  // Activation remains on the message thread. Offline state restoration and
  // processing may share another thread under the VST3 contract.
  instance->setNonRealtime(true);
  instance->prepareToPlay(48'000.0, 8);
  auto &component = *instance->getVST3Client()->getIComponentPtr();
  Vst::IAudioProcessor *raw{};
  if (component.queryInterface(Vst::IAudioProcessor::iid,
                               reinterpret_cast<void **>(&raw)) != kResultOk)
    return false;
  std::unique_ptr<Vst::IAudioProcessor, ReleaseInterface> processor{raw};
  MemoryStream state;
  if (component.getState(&state) != kResultOk)
    return false;

  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  std::array<float *, 64> pointers{};
  for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    pointers[static_cast<std::size_t>(channel)] =
        audio.getWritePointer(channel);
  std::array<Vst::AudioBusBuffers, pluginInputBuses> inputs{};
  if constexpr (pluginInputChannels > 0) {
    inputs[0].numChannels = pluginInputChannels;
    inputs[0].channelBuffers32 = pointers.data();
  }
  Vst::AudioBusBuffers output{};
  output.numChannels = pluginOutputChannels;
  output.channelBuffers32 = pointers.data();
  constexpr auto exact = 0.625 + 0x1p-40;
  const std::array points{Point{0, exact}, Point{7, exact}};
  const std::array controlPoints{Point{0, 0.75}};
  ValueQueue precise{"slot01", points}, control{"input01", controlPoints};
  const std::array queues{&precise, &control};
  Changes changes{std::span<ValueQueue *const>{queues}.first(
      pluginControlInputs > 0 ? 2U : 1U)};
  Vst::ProcessData data{};
  data.processMode = Vst::kOffline;
  data.symbolicSampleSize = Vst::kSample32;
  data.numSamples = 8;
  data.numInputs = pluginInputBuses;
  data.numOutputs = 1;
  data.inputs = inputs.data();
  data.outputs = &output;
  auto succeeded = true;
  std::thread offline([&] {
    for (const auto automate : {false, true}) {
      if (state.seek(0, IBStream::kIBSeekSet, nullptr) != kResultOk ||
          component.setState(&state) != kResultOk) {
        succeeded = false;
        return;
      }
      OutputChanges outgoing;
      data.outputParameterChanges = &outgoing;
      data.inputParameterChanges = automate ? &changes : nullptr;
      audio.clear();
      if (processor->process(data) != kResultOk || outgoing.count != 0) {
        std::cerr << instance->getName() << " offline restore "
                  << (automate ? "with" : "without") << " automation emitted "
                  << outgoing.count << " queues\n";
        succeeded = false;
        return;
      }
      const auto expected =
          (automate ? 1.0F : 0.0F) +
          (pluginControlInputs > 0 ? (automate ? 0.75F : 0.25F) : 0.0F);
      for (int frame = 0; frame < 8; ++frame)
        if (!test::withinTolerance(audio.getSample(0, frame) - expected,
                                   1.0e-6F)) {
          succeeded = false;
          return;
        }
    }
    data.outputParameterChanges = nullptr;
    data.inputParameterChanges = nullptr;
  });
  offline.join();
  if (succeeded) {
    succeeded = controller.connect(component, module);
  }
  if (succeeded) {
    // Deliver the pending notifications and verify that the next hosted
    // callback keeps the exact f64 endpoint and control value.
    succeeded = test::waitUntil(
        [&] {
          return controller.value("slot01") == static_cast<float>(exact) &&
                 (pluginControlInputs == 0 ||
                  controller.value("input01") == 0.75);
        },
        [] { juce::MessageManager::getInstance()->runDispatchLoopUntil(10); });
    succeeded &= controller.edits == 0 && controller.gestures == 0;
    juce::MidiBuffer midi;
    instance->processBlock(audio, midi);
    const auto expected = pluginControlInputs > 0 ? 1.75F : 1.0F;
    for (int frame = 0; frame < 8; ++frame)
      succeeded &=
          test::withinTolerance(audio.getSample(0, frame) - expected, 1.0e-6F);
  }
  instance->releaseResources();
  return succeeded;
}

bool smokeAutomation(juce::AudioPluginInstance &instance) {
  const auto source = juce::File{
      onda::plugin::pathToJuce(testTemporaryRoot() / "automation.onda")};
  constexpr auto program =
#if ONDA_PLUGIN_OUTPUT_CHANNELS >= 2
      "outs { out1, out2 }\n"
#else
      "outs { out1 }\n"
#endif
      R"(
params {
  level = 0.0 {0.0, 1.0}
  precise: f64 = 1.0 {1.0, 100.0, log}
  gate: bool = false
}
init { held = 1.0; blocks = 0 }
event note_on(id: i32, channel: i32, key: i32, velocity: f32) { held = velocity }
event note_off(id: i32, channel: i32, key: i32, velocity: f32) { held = 0.0 }
block {
  blocks += 1
  cached_level = level
  cached_precise = precise
  cached_gate = gate
  sample {
    out1 = cached_level * held
)"
#if ONDA_PLUGIN_OUTPUT_CHANNELS >= 2
      R"(
    value = f32(cached_precise) + f32(blocks) * 128.0
    if cached_gate { value += 2.0 }
    out2 = value
)"
#endif
      R"(
  }
}
)";
  if (!source.replaceWithText(program))
    return false;
  setOndaProgram(instance, source, 0.0F, 2);
  instance.setNonRealtime(false);
  instance.prepareToPlay(48'000.0, 8);
  auto *client = instance.getVST3Client();
  Vst::IAudioProcessor *raw{};
  if (client == nullptr || client->getIComponentPtr()->queryInterface(
                               Vst::IAudioProcessor::iid,
                               reinterpret_cast<void **>(&raw)) != kResultOk)
    return false;
  std::unique_ptr<Vst::IAudioProcessor, ReleaseInterface> processor{raw};

  const auto channels =
      std::max(ONDA_PLUGIN_INPUT_CHANNELS, ONDA_PLUGIN_OUTPUT_CHANNELS);
  juce::AudioBuffer<float> audio(channels, 8);
  std::array<float *, 64> pointers{};
  for (int channel = 0; channel < channels; ++channel)
    pointers[static_cast<std::size_t>(channel)] =
        audio.getWritePointer(channel);
  std::array<Vst::AudioBusBuffers, onda::plugin::pluginInputBuses> inputs{};
  if constexpr (ONDA_PLUGIN_INPUT_CHANNELS > 0) {
    inputs[0].numChannels = ONDA_PLUGIN_INPUT_CHANNELS;
    inputs[0].channelBuffers32 = pointers.data();
  }
  Vst::AudioBusBuffers output{};
  output.numChannels = ONDA_PLUGIN_OUTPUT_CHANNELS;
  output.channelBuffers32 = pointers.data();
  Vst::ProcessData data{};
  data.processMode = Vst::kRealtime;
  data.symbolicSampleSize = Vst::kSample32;
  data.numSamples = 8;
  data.numInputs = onda::plugin::pluginInputBuses;
  data.numOutputs = 1;
  data.inputs = inputs.data();
  data.outputs = &output;

  const auto render = [&](Changes &changes, const std::array<float, 8> &levels,
                          const int gateAt, const int priorBlocks,
                          const bool fallingGate = false) {
    data.inputParameterChanges = &changes;
    audio.clear();
    if (processRealtime(*processor, data) != kResultOk)
      return false;
    for (int frame = 0; frame < 8; ++frame) {
      // JUCE represents VST3 notes as MIDI 1 messages, quantizing velocity.
      const auto held =
          data.inputEvents != nullptr && frame >= 3 ? 64.0F / 127.0F : 1.0F;
      const auto levelValue = levels[static_cast<std::size_t>(frame)];
      const auto gateValue = ONDA_PLUGIN_PARAMETER_SLOTS >= 3 &&
                             ((frame >= gateAt) != fallingGate);
      const auto expectedBlocks = priorBlocks + frame / 2 + 1;
      const auto right = (ONDA_PLUGIN_PARAMETER_SLOTS >= 2
                              ? static_cast<float>(std::pow(100.0, levelValue))
                              : 1.0F) +
                         (gateValue ? 2.0F : 0.0F) +
                         static_cast<float>(expectedBlocks) * 128.0F;
      const auto rightActual =
          ONDA_PLUGIN_OUTPUT_CHANNELS >= 2 ? audio.getSample(1, frame) : 0.0F;
      if (!test::withinTolerance(audio.getSample(0, frame) - levelValue * held,
                                 1.0e-6F) ||
          (ONDA_PLUGIN_OUTPUT_CHANNELS >= 2 &&
           !test::withinTolerance(rightActual - right, 1.0e-4F))) {
        std::cerr << instance.getName()
                  << " failed block-rate VST3 automation at sample " << frame
                  << " (left " << audio.getSample(0, frame) << ", expected "
                  << levelValue * held << ", right " << rightActual
                  << ", expected " << right << ")\n";
        return false;
      }
    }
    return true;
  };

  // All points reach DSP, but values change only at the chosen two-sample
  // Onda boundaries. Block code executes exactly four times per callback.
  const std::array ramp{Point{0, 0.0}, Point{3, 1.0}, Point{4, 0.25},
                        Point{7, 0.75}};
  const std::array on{Point{4, 1.0}};
  ValueQueue level{"slot01", ramp}, precise{"slot02", ramp}, gate{"slot03", on};
  const std::array firstQueues{&level, &precise, &gate};
  Changes first{std::span<ValueQueue *const>{firstQueues}.first(
      std::min(std::size_t{ONDA_PLUGIN_PARAMETER_SLOTS}, firstQueues.size()))};
  if (!render(first,
              {0, 0, 2.0F / 3, 2.0F / 3, .25F, .25F, 7.0F / 12, 7.0F / 12}, 4,
              0))
    return false;

  const std::array descending{Point{7, 0.0}};
  const std::array off{Point{6, 0.0}};
  ValueQueue down{"slot01", descending}, downPrecise{"slot02", descending},
      offGate{"slot03", off};
  const std::array secondQueues{&down, &downPrecise, &offGate};
  Changes second{std::span<ValueQueue *const>{secondQueues}.first(
      std::min(std::size_t{ONDA_PLUGIN_PARAMETER_SLOTS}, secondQueues.size()))};
  if (!render(second,
              {.65625F, .65625F, .46875F, .46875F, .28125F, .28125F, .09375F,
               .09375F},
              6, 4, true))
    return false;
  Changes empty{std::span<ValueQueue *const>{}};
  if (!render(empty, {}, 8, 8))
    return false;

  // MIDI keeps its sample offset without restarting Onda's logical block.
  const std::array ascending{Point{7, 1.0}};
  ValueQueue up{"slot01", ascending}, upPrecise{"slot02", ascending};
  const std::array thirdQueues{&up, &upPrecise};
  Changes third{std::span<ValueQueue *const>{thirdQueues}.first(
      std::min(std::size_t{ONDA_PLUGIN_PARAMETER_SLOTS}, thirdQueues.size()))};
  Events events;
  events.note.type = Vst::Event::kNoteOnEvent;
  events.note.sampleOffset = 3;
  events.note.noteOn = {0, 60, 0.0F, 0.5F, 0, -1};
  data.inputEvents = &events;
  if (!render(third, {.125F, .125F, .375F, .375F, .625F, .625F, .875F, .875F},
              8, 12))
    return false;
  instance.releaseResources();
  return true;
}

bool smokeCallbackProcessing(juce::AudioPluginInstance &instance) {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "vst3-callback-processing.onda")};
  if (!source.replaceWithText(test::callbackProcessingSource()))
    return false;
  for (const auto block : {1, 3, 8})
    for (const auto routed : {false, true}) {
      if (routed && pluginControlInputs == 0)
        continue;
      if (!test::disableControlInputBuses(instance))
        return false;
      if (routed) {
        auto layout = instance.getBusesLayout();
        layout.inputBuses.set(pluginMainInputBuses,
                              juce::AudioChannelSet::mono());
        if (!instance.setBusesLayout(layout))
          return false;
      }
      setOndaProgram(instance, source, 0.0F, block);
      instance.setNonRealtime(false);
      instance.prepareToPlay(1000.0, 8);
      auto *client = instance.getVST3Client();
      Vst::IAudioProcessor *raw{};
      if (client == nullptr ||
          client->getIComponentPtr()->queryInterface(
              Vst::IAudioProcessor::iid, reinterpret_cast<void **>(&raw)) !=
              kResultOk)
        return false;
      std::unique_ptr<Vst::IAudioProcessor, ReleaseInterface> processor{raw};
      if (!test::checkCallbackProcessing(
              instance, block, routed,
              [&](juce::AudioBuffer<float> &audio, juce::MidiBuffer &midi,
                  std::span<const AutomationPoint> level,
                  std::span<const AutomationPoint> control) {
                const std::array parameterPoints{
                    Point{level[0].offset, level[0].value},
                    Point{level[1].offset, level[1].value}};
                const std::array controlPoints{
                    Point{control[0].offset, control[0].value},
                    Point{control[1].offset, control[1].value}};
                ValueQueue parameterQueue{"slot01", parameterPoints},
                    controlQueue{"input01", controlPoints};
                const std::array queues{&parameterQueue, &controlQueue};
                Changes changes{std::span<ValueQueue *const>{queues}.first(
                    pluginControlInputs > 0 ? 2U : 1U)};
                std::array<float *, 64> pointers{};
                for (int channel = 0; channel < audio.getNumChannels();
                     ++channel)
                  pointers[static_cast<std::size_t>(channel)] =
                      audio.getWritePointer(channel);
                std::array<Vst::AudioBusBuffers, pluginInputBuses> inputs{};
                if constexpr (pluginInputChannels > 0) {
                  inputs[0].numChannels = pluginInputChannels;
                  inputs[0].channelBuffers32 = pointers.data();
                }
                if (routed) {
                  inputs[pluginMainInputBuses].numChannels = 1;
                  inputs[pluginMainInputBuses].channelBuffers32 =
                      pointers.data() + pluginInputChannels;
                }
                Vst::AudioBusBuffers output{};
                output.numChannels = pluginOutputChannels;
                output.channelBuffers32 = pointers.data();
                const auto event = *midi.begin();
                Events events;
                events.note.sampleOffset = event.samplePosition;
                events.note.type = event.getMessage().isNoteOn()
                                       ? Vst::Event::kNoteOnEvent
                                       : Vst::Event::kNoteOffEvent;
                if (events.note.type == Vst::Event::kNoteOnEvent)
                  events.note.noteOn = {0, 60, 0.0F, 1.0F, 0, -1};
                else
                  events.note.noteOff = {0, 60, 0.0F, -1, 0.0F};
                Vst::ProcessData data{};
                data.processMode = Vst::kRealtime;
                data.symbolicSampleSize = Vst::kSample32;
                data.numSamples = audio.getNumSamples();
                data.numInputs = pluginInputBuses;
                data.numOutputs = 1;
                data.inputs = inputs.data();
                data.outputs = &output;
                data.inputParameterChanges = &changes;
                data.inputEvents = &events;
                return processRealtime(*processor, data) == kResultOk;
              }))
        return false;
      instance.releaseResources();
    }
  return test::disableControlInputBuses(instance);
}

bool smokeControlAutomation(juce::AudioPluginInstance &instance) {
  using namespace onda::plugin;
  if constexpr (pluginControlInputs == 0)
    return true;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "vst3-control-automation.onda")};
  auto program = "ins {\n" + test::mainAudioInputDeclaration() +
                 "  modulation = -1.0 {-2.0, 2.0}\n";
  if constexpr (pluginControlInputs > 1)
    program += "  secondary = 0.0 {0.0, 1.0}\n";
  program += "}\nparams { offset = 0.0 {0.0, 1.0} }\nouts { out1";
  if constexpr (pluginOutputChannels > 1)
    program += ", out2";
  program += "}\ninit { blocks = 0; held = 1.0 }\n"
             "event note_on(id: i32, channel: i32, key: i32, velocity: f32) { "
             "held = 0.5 }\n"
             "event note_off(id: i32, channel: i32, key: i32, velocity: f32) { "
             "held = 1.0 }\n"
             "block { blocks += 1; cached_offset = offset; sample { out1 = "
             "(modulation + cached_offset";
  if constexpr (pluginControlInputs > 1)
    program += " + secondary * 4.0";
  program += ") * held\n";
  if constexpr (pluginOutputChannels > 1)
    program += "out2 = f32(blocks)\n";
  program += "} }\n";
  if (!source.replaceWithText(program))
    return false;
  instance.disableNonMainBuses();
  for (const int block : {8, 3, 1}) {
    const std::array initial{0.25F, 0.0F};
    setOndaProgram(instance, source, 0.0F, block, initial);
    instance.prepareToPlay(48'000.0, 8);
    if (!test::waitUntil(
            [&] {
              return instance.getParameters()[ONDA_PLUGIN_PARAMETER_SLOTS]->getName(128) == "modulation";
            },
            [] {
              juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
            })) {
      std::cerr << "VST3 control parameter title did not update\n";
      return false;
    }
    auto *client = instance.getVST3Client();
    Vst::IAudioProcessor *raw{};
    if (client == nullptr || client->getIComponentPtr()->queryInterface(
                                 Vst::IAudioProcessor::iid,
                                 reinterpret_cast<void **>(&raw)) != kResultOk)
      return false;
    std::unique_ptr<Vst::IAudioProcessor, ReleaseInterface> processor{raw};
    Vst::BusInfo busInfo{};
    if (client->getIComponentPtr()->getBusInfo(Vst::kAudio, Vst::kInput,
                                               pluginMainInputBuses,
                                               busInfo) != kResultOk ||
        juce::String{juce::CharPointer_UTF16{
            reinterpret_cast<const juce::CharPointer_UTF16::CharType *>(
                busInfo.name)}} != "modulation")
      return false;

    juce::AudioBuffer<float> audio(
        std::max(pluginInputChannels, pluginOutputChannels), 8);
    std::array<float *, 64> pointers{};
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
      pointers[static_cast<std::size_t>(channel)] =
          audio.getWritePointer(channel);
    std::array<Vst::AudioBusBuffers, pluginInputBuses> inputs{};
    if constexpr (pluginInputChannels > 0) {
      inputs[0].numChannels = pluginInputChannels;
      inputs[0].channelBuffers32 = pointers.data();
    }
    Vst::AudioBusBuffers output{};
    output.numChannels = pluginOutputChannels;
    output.channelBuffers32 = pointers.data();
    Vst::ProcessData data{};
    data.processMode = Vst::kRealtime;
    data.symbolicSampleSize = Vst::kSample32;
    data.numSamples = 8;
    data.numInputs = pluginInputBuses;
    data.numOutputs = 1;
    data.inputs = inputs.data();
    data.outputs = &output;
    int renderedCallbacks{};
    bool noteHeld{};
    const auto render = [&](Changes &changes,
                            const std::array<float, 8> &values) {
      audio.clear();
      data.inputParameterChanges = &changes;
      if (processRealtime(*processor, data) != kResultOk)
        return false;
      for (int frame = 0; frame < 8; ++frame) {
        if (data.inputEvents != nullptr && frame >= 3)
          noteHeld = true;
        const auto held = noteHeld ? 0.5F : 1.0F;
        const auto boundary = frame / block * block;
        const auto offset = renderedCallbacks == 0
                                ? static_cast<float>(boundary) / 14.0F
                                : 0.5F;
        const auto secondary =
            pluginControlInputs < 2
                ? 0.0F
                : (renderedCallbacks == 0 ? 0.4F * static_cast<float>(frame + 1)
                                          : 3.2F);
        const auto expected =
            (values[static_cast<std::size_t>(frame)] + offset + secondary) *
            held;
        const auto blocks = static_cast<float>(
            renderedCallbacks * ((8 + block - 1) / block) + frame / block + 1);
        if (!test::withinTolerance(audio.getSample(0, frame) - expected,
                                   1.0e-6F) ||
            (pluginOutputChannels > 1 &&
             !test::withinTolerance(audio.getSample(1, frame) - blocks,
                                    1.0e-6F))) {
          std::cerr << instance.getName() << " VST3 control automation block "
                    << block << " sample " << frame << " actual "
                    << audio.getSample(0, frame) << " expected " << expected
                    << '\n';
          return false;
        }
      }
      ++renderedCallbacks;
      return true;
    };
    const std::array points{Point{0, 0.0}, Point{3, 1.0}, Point{4, 0.25},
                            Point{7, 0.75}};
    ValueQueue curve{"input01", points};
    const std::array ordinaryPoints{Point{0, 0.0}, Point{7, 0.5}};
    const std::array secondaryPoints{Point{7, 0.8}};
    ValueQueue ordinary{"slot01", ordinaryPoints},
        secondary{"input02", secondaryPoints};
    const std::array queues{&curve, &ordinary, &secondary};
    Changes changes{std::span<ValueQueue *const>{queues}.first(
        pluginControlInputs > 1 ? 3U : 2U)};
    if (!render(changes,
                {-2, -2.0F / 3, 2.0F / 3, 2, -1, -1.0F / 3, 1.0F / 3, 1}))
      return false;
    // VST3 parameter flushes must publish their endpoint with zero samples.
    const std::array flushPoints{Point{0, 0.25}};
    ValueQueue flushQueue{"input01", flushPoints};
    const std::array flushQueues{&flushQueue};
    Changes flush{flushQueues};
    data.numSamples = 0;
    data.inputParameterChanges = &flush;
    if (processRealtime(*processor, data) != kResultOk)
      return false;
    data.numSamples = 8;
    const std::array descending{Point{7, 0.0}};
    ValueQueue down{"input01", descending};
    const std::array nextQueues{&down};
    Changes next{nextQueues};
    Events events;
    events.note.type = Vst::Event::kNoteOnEvent;
    events.note.sampleOffset = 3;
    events.note.noteOn = {0, 60, 0.0F, 1.0F, 0, -1};
    data.inputEvents = &events;
    if (!render(next, {-1.125F, -1.25F, -1.375F, -1.5F, -1.625F, -1.75F,
                       -1.875F, -2}))
      return false;
    data.inputEvents = nullptr;
    Changes empty{std::span<ValueQueue *const>{}};
    if (!render(empty, {-2, -2, -2, -2, -2, -2, -2, -2}))
      return false;
    instance.releaseResources();
  }
  return true;
}
