#include "Vst3AutomationTests.h"

#include "JucePath.h"
#include "TemporaryDirectory.h"
#include "TestWait.h"
#include "Vst3ControllerTestSupport.h"
#include "Vst3TestSupport.h"

#include <iostream>

bool smokeEnumParameters(juce::VST3PluginFormatHeadless &format,
                         const juce::PluginDescription &description,
                         const juce::File &module) {
  using namespace Steinberg;
  using namespace onda::plugin;
  vst3_test::ObservedController controller;
  juce::String error;
  auto instance =
      format.createInstanceFromDescription(description, 48'000.0, 8, error);
  if (instance == nullptr)
    return false;
  const juce::File source{pathToJuce(testTemporaryRoot() / "vst3-enums.onda")};
  const auto program = [](const juce::String &label) {
    return "enum Mode { LowPass; HighPass; BandPass {label = \"" + label +
           "\"} }\n" + R"(
enum One { Only }
params {
  mode = Mode::HighPass
  fixed = One::Only
  modes: Mode[2] = [Mode::LowPass, Mode::BandPass]
  tail = 0.5 {0.0, 1.0}
}
outs { out1 }
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}
sample { out1 = 0.0 }
)";
  };
  const auto wait = [](auto predicate) {
    return test::waitUntil(predicate, [] {
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    });
  };
  if (!source.replaceWithText(program("Band-pass")) ||
      !instance->disableNonMainBuses())
    return false;
  setOndaProgram(*instance, source);
  instance->prepareToPlay(48'000.0, 8);
  if (!controller.connect(*instance->getVST3Client()->getIComponentPtr(),
                          module) ||
      !wait([&] { return controller.text("slot01", 1.0) == "Band-pass"; }))
    return false;
  const auto checkFlags = [&](const char *id, const int32 required,
                              const int32 forbidden) {
    const auto info = controller.parameterInfo(id);
    return info && (info->flags & required) == required &&
           (info->flags & forbidden) == 0;
  };
  if (controller.stepCount("slot01") != 2 ||
      controller.text("slot01", 0.0) != "Low Pass" ||
      controller.text("slot01", 0.5) != "High Pass" ||
      controller.valueForText("slot01", "Band-pass") != 1.0 ||
      controller.valueForText("slot01", "HighPass") != 0.5 ||
      !checkFlags("slot01",
                  Vst::ParameterInfo::kIsList |
                      Vst::ParameterInfo::kCanAutomate,
                  Vst::ParameterInfo::kIsReadOnly))
    return false;
  if constexpr (ONDA_PLUGIN_PARAMETER_SLOTS > 1) {
    if (controller.stepCount("slot02") != 0 ||
        controller.text("slot02", 1.0) != "Only" ||
        !checkFlags("slot02",
                    Vst::ParameterInfo::kIsList |
                        Vst::ParameterInfo::kIsReadOnly,
                    Vst::ParameterInfo::kCanAutomate))
      return false;
  }
  if constexpr (ONDA_PLUGIN_PARAMETER_SLOTS > 3)
    if (controller.text("slot03", 1.0) != "Band-pass" ||
        controller.stepCount("slot04") != 2)
      return false;
  if constexpr (ONDA_PLUGIN_PARAMETER_SLOTS > 4)
    if (!checkFlags("slot05", Vst::ParameterInfo::kCanAutomate,
                    Vst::ParameterInfo::kIsList))
      return false;

  const auto refreshes = controller.titleRefreshes;
  if (!source.replaceWithText(program("Band select")))
    return false;
  setOndaProgram(*instance, source);
  instance->prepareToPlay(48'000.0, 8);
  if (!wait([&] {
        return controller.titleRefreshes > refreshes &&
               controller.text("slot01", 1.0) == "Band select";
      })) {
    std::cerr << "VST3 did not invalidate labels after an enum-only reload\n";
    return false;
  }

  for (const auto *label : {"LowPass", "low pass", " LowPass "}) {
    if (!source.replaceWithText(program(label)))
      return false;
    setOndaProgram(*instance, source);
    instance->prepareToPlay(48'000.0, 8);
    if (!wait([&] { return controller.text("slot01", 1.0) == label; }) ||
        controller.valueForText("slot01", label) != 1.0 ||
        controller.valueForText("slot01", "Low Pass") != 0.0 ||
        controller.valueForText("slot01", "HighPass") != 0.5) {
      std::cerr << "VST3 enum labels did not round-trip before aliases\n";
      return false;
    }
  }

  auto numeric = program("Band select");
  numeric = numeric.replace("mode = Mode::HighPass", "mode: i32 = 1 {0, 2}");
  numeric = numeric.replace("enum One { Only }", "enum One { Only; Another }");
  if (!source.replaceWithText(numeric))
    return false;
  setOndaProgram(*instance, source);
  instance->prepareToPlay(48'000.0, 8);
  if (!wait([&] {
        return controller.text("slot01", 1.0) == "2" &&
               checkFlags("slot01", Vst::ParameterInfo::kCanAutomate,
                          Vst::ParameterInfo::kIsList);
      }))
    return false;
  if constexpr (ONDA_PLUGIN_PARAMETER_SLOTS > 1)
    if (!checkFlags("slot02",
                    Vst::ParameterInfo::kIsList |
                        Vst::ParameterInfo::kCanAutomate,
                    Vst::ParameterInfo::kIsReadOnly))
      return false;
  instance->releaseResources();
  return true;
}
