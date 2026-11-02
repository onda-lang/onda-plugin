#include "EnumParameterTests.h"

#include "HostParameter.h"
#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "RunViewHost.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"

#include <iostream>

namespace {
using namespace onda::plugin;

bool check(const bool condition, const char *message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

bool render(Processor &processor, const float expected) {
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  juce::MidiBuffer midi;
  audio.clear();
  const auto processed = test::processRealtime(processor, audio, midi);
  if (!processed ||
      !test::withinTolerance(audio.getSample(0, 0) - expected, 1.0e-6F)) {
    std::cerr << "enum render: " << audio.getSample(0, 0) << ", expected "
              << expected << "; " << processor.workerStatus().message << '\n';
    return false;
  }
  return true;
}

juce::String enumDefinition(const juce::String &label = "Band-pass") {
  return "enum Mode { LowPass; HighPass; BandPass {label = \"" + label +
         "\"} }\n";
}

} // namespace

bool exerciseEnumParameters() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "enum-parameters.onda")};
  const auto program = [](const juce::String &label) {
    return enumDefinition(label) + R"(
enum One { Only }
params {
  mode = Mode::HighPass
  fixed = One::Only
  modes: Mode[2] = [Mode::LowPass, Mode::BandPass]
  tail = 0.25 {0.0, 1.0}
}
outs { out1 }
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}
sample { out1 = i32(mode) + i32(modes[0]) + i32(modes[1]) + tail }
)";
  };
  for (const auto product : {Product::effect, Product::instrument}) {
    if (!source.replaceWithText(program("Band-pass")))
      return false;
    Processor processor(product);
    processor.setNonRealtime(true);
    processor.loadFile(source);
    processor.prepareToPlay(48'000.0, 8);
    test::service(processor);
    if (!render(processor, 3.25F))
      return false;
    const auto &parameters = processor.getParameters();
    const auto *mode = parameters[0];
    if (!check(mode->isDiscrete() && !mode->isBoolean() &&
                   mode->getNumSteps() == 3 && mode->isAutomatable() &&
                   mode->getText(0.0F, 128) == "Low Pass" &&
                   mode->getText(0.5F, 128) == "High Pass" &&
                   mode->getText(1.0F, 128) == "Band-pass" &&
                   mode->getText(1.0F, 4) == "Band" &&
                   std::is_eq(mode->getValueForText("High Pass") <=> 0.5F) &&
                   std::is_eq(mode->getValueForText("BandPass") <=> 1.0F) &&
                   std::is_eq(mode->getValueForText("band-pass") <=> 1.0F) &&
                   std::is_eq(mode->getValueForText("2") <=> 1.0F),
               "enum host value labels or conversions were invalid"))
      return false;
    const auto state = makeRunViewState(processor, processor.workerStatus(),
                                        false, {})["state"];
    const auto &params = state["params"];
    if (!check(params.size() ==
                       static_cast<int>(std::min<std::size_t>(slotCount, 5U)) &&
                   params[0]["type"] == "i32" &&
                   params[0]["enumeration"]["name"] == "Mode" &&
                   params[0]["enumeration"]["members"][2]["label"] ==
                       "Band-pass",
               "enum parameter metadata did not reach the shared view"))
      return false;
    if constexpr (slotCount > 1) {
      const auto *fixed = parameters[1];
      if (!check(fixed->isDiscrete() && !fixed->isAutomatable() &&
                     fixed->getNumSteps() == 1 &&
                     fixed->getText(1.0F, 128) == "Only" &&
                     std::is_eq(fixed->getValueForText("Only") <=> 0.0F) &&
                     static_cast<int>(params[1]["stepCount"]) == 0,
                 "a singleton enum was omitted or treated as continuous"))
        return false;
    }
    if constexpr (slotCount > 3) {
      if (!check(parameters[2]->getName(128).startsWith("modes[0]") &&
                     parameters[3]->getText(1.0F, 128) == "Band-pass" &&
                     params[2]["enumeration"]["members"].size() == 3 &&
                     static_cast<int>(params[3]["array"]["index"]) == 1,
                 "enum arrays lost their choices or stable scalar slots"))
        return false;
    }
    processor.setSlotValue(0, 1.0F);
    if (!render(processor, 4.25F))
      return false;
    auto expected = 4.25F;
    if constexpr (slotCount > 2) {
      processor.setSlotValue(2, 0.5F);
      expected += 1.0F;
      if (!render(processor, expected))
        return false;
    }
    if (!source.replaceWithText(program("Band select")))
      return false;
    processor.loadFile(source, false);
    processor.prepareToPlay(48'000.0, 8);
    test::service(processor);
    if (!render(processor, expected) ||
        !check(mode->getText(1.0F, 128) == "Band select",
               "reloading enum labels did not refresh host presentation"))
      return false;
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    if (!source.deleteFile())
      return false;
    Processor restored(product);
    restored.setNonRealtime(true);
    restored.setStateInformation(saved.getData(),
                                 static_cast<int>(saved.getSize()));
    restored.prepareToPlay(48'000.0, 8);
    test::service(restored);
    if (!render(restored, expected) ||
        !check(restored.getParameters()[0]->getText(1.0F, 128) == "Band select",
               "saved enum choices did not survive image restoration"))
      return false;
  }
  return true;
}

bool exerciseEnumEventMetadata() {
  using namespace onda::plugin;
  const juce::File source{pathToJuce(testTemporaryRoot() / "enum-events.onda")};
  if (!source.replaceWithText(enumDefinition() + R"(
struct Settings {
  mode: Mode
  modes: Mode[2]
  pair: (Mode, i32)
}
init { observed = 0.0 }
delegate changed(settings: Settings, values: Mode[])
event configure(settings: Settings, values: Mode[]) {
  observed = i32(settings.mode) + i32(settings.modes[0]) + i32(settings.modes[1])
  observed += i32(settings.pair[0]) + settings.pair[1] + i32(values[0])
  changed(settings, values)
}
sample { out1 = observed }
)"))
    return false;
  Processor processor(Product::effect);
  processor.setNonRealtime(true);
  processor.loadFile(source);
  processor.prepareToPlay(48'000.0, 8);
  test::service(processor);
  if (!render(processor, 0.0F))
    return false;
  const auto state =
      makeRunViewState(processor, processor.workerStatus(), false, {})["state"];
  const auto args = state["events"][0]["args"];
  const auto fields = args[0]["shape"]["fields"];
  for (const auto &shape :
       {fields[0]["ty"], fields[1]["ty"]["element"],
        fields[2]["ty"]["elements"][0], args[1]["shape"]["element"]}) {
    if (!check(shape["encoding"] == "i32" &&
                   shape["enumeration"]["members"][2]["label"] == "Band-pass",
               "nested event enum choices were lost during schema forwarding"))
      return false;
  }
  const auto values =
      juce::JSON::parse(R"([{"mode":99,"modes":[-99,1],"pair":[99,7]},[-99]])");
  const auto error = processor.triggerEvent(
      processor.workerStatus().engineGeneration, "configure", values);
  if (!check(error.empty(), error.c_str()) || !render(processor, 12.0F))
    return false;
  test::service(processor);
  return check(!processor.runtimeLogSnapshot().records.empty(),
               "enum delegate payload did not retain its i32 transport");
}
