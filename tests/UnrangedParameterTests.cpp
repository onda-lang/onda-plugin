#include "UnrangedParameterTests.h"

#include "ControlInputTestSupport.h"
#include "EditorTestSupport.h"
#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "RunViewHost.h"
#include "StateFormat.h"
#include "TemporaryDirectory.h"
#include "TestAutomationQueue.h"

#include <iostream>

namespace {
using namespace onda::plugin;

juce::String program(const juce::String &declaration,
                     const bool withParameter = true) {
  auto text = "ins {\n" + test::mainAudioInputDeclaration();
  if constexpr (pluginControlInputs > 0)
    text += "cv = 1.0 {0.0, 1.0}\n";
  text += "}\nouts { out }\n"
          "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n";
  if (withParameter)
    text += "params { " + declaration + " }\n";
  text += "sample { out = ";
  text += withParameter
              ? (declaration.contains("bool") ? "f32(i32(test))" : "f32(test)")
              : "1.0";
  if constexpr (pluginControlInputs > 0)
    text += " * cv";
  return text + " }\n";
}

bool render(Processor &processor, const float expected) {
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  audio.clear();
  juce::MidiBuffer midi;
  if (!test::processRealtime(processor, audio, midi))
    return false;
  for (int frame = 0; frame < audio.getNumSamples(); ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) - expected, 1.0e-6F)) {
      std::cerr << "Unranged parameter rendered " << audio.getSample(0, frame)
                << ", expected " << expected << '\n';
      return false;
    }
  return true;
}

bool roundTrip(Processor &processor, Product product, float expected) {
  juce::MemoryBlock state;
  processor.getStateInformation(state);
  Processor restored(product);
  restored.setStateInformation(state.getData(),
                               static_cast<int>(state.getSize()));
  restored.prepareToPlay(1000.0, 8);
  return render(restored, expected);
}

bool check(Product product, const char *type) {
  const auto source =
      juce::File(pathToJuce(testTemporaryRoot() / "unranged-parameter.onda"));
  if (!source.replaceWithText(program("test: " + juce::String{type})))
    return false;
  Processor processor(product);
  processor.loadFile(source);
  processor.prepareToPlay(1000.0, 8);
  test::service(processor);
  const auto status = processor.workerStatus();
  const auto state = makeRunViewState(processor, status, true, {})["state"];
  const auto *parameters = state["params"].getArray();
  if (status.mappings.size() != 1U || status.mappings[0].name != "test" ||
      parameters == nullptr || parameters->size() != 1 ||
      parameters->getReference(0)["rangeMin"] != juce::var{0.0} ||
      parameters->getReference(0)["rangeMax"] != juce::var{1.0} ||
      processor.getParameters()[0]->getName(128) != "test [Slot 1]") {
    std::cerr << "Unranged " << type << " parameter is missing from Params\n";
    return false;
  }
  auto compiled =
      compileFile(pathFromJuce(source.getFullPathName()), 1000.0, 8);
  if (!compiled.program || onda_param_has_range(compiled.program.get(), 0) != 0)
    return false;

  const auto discrete =
      juce::String{type}.startsWithChar('i') || juce::String{type} == "bool";
  auto *parameter = processor.getParameters()[0];
  if (parameter->isDiscrete() != discrete || !render(processor, 0.0F))
    return false;
  parameter->setValueNotifyingHost(0.75F);
  const auto plain = discrete ? 1.0F : 0.75F;
  if (!render(processor, plain) || !roundTrip(processor, product, plain))
    return false;

  if (!processor.setParamSmoothingSeconds(0.016))
    return false;
  Editor editor(processor);
  auto command = juce::var{new juce::DynamicObject};
  command.getDynamicObject()->setProperty("type", "setParam");
  command.getDynamicObject()->setProperty("name", "Slot 1 -> test");
  command.getDynamicObject()->setProperty("value", 0.0);
  command.getDynamicObject()->setProperty("commit", true);
  EditorTestAccess::command(editor, command);
  if (!render(processor, discrete ? 0.0F : 0.375F) || !render(processor, 0.0F))
    return false;

  const std::array points{AutomationPoint{0, 0.75}};
  TestAutomationQueue queue{points};
  const auto id = juce::VST3ClientExtensions::convertJuceParameterId("slot01");
  if (!test::captureRealtime(processor, id, queue.queue(), 8) ||
      !render(processor, plain))
    return false;
  processor.resetParametersToDefaults(ParameterReset::parameters);
  return render(processor, 0.0F);
}

bool reload(Product product) {
  const auto source = juce::File(
      pathToJuce(testTemporaryRoot() / "unranged-parameter-reload.onda"));
  if (!source.replaceWithText(program({}, false)))
    return false;
  Processor processor(product);
  processor.loadFile(source);
  processor.prepareToPlay(1000.0, 8);
  if (!render(processor, 1.0F))
    return false;
  const auto previous = processor.workerStatus().engineGeneration;
  const auto retained = processor.slotValue(0);
  if (!source.replaceWithText(program("test")) ||
      !test::waitUntil(
          [&] {
            const auto status = processor.workerStatus();
            return status.active && !status.compiling &&
                   status.engineGeneration != previous;
          },
          test::yieldToWorker)) {
    std::cerr << "Adding an unranged parameter did not trigger reload\n";
    return false;
  }
  test::service(processor);
  const auto state =
      makeRunViewState(processor, processor.workerStatus(), true, {})["state"];
  const auto *parameters = state["params"].getArray();
  if (parameters == nullptr || parameters->size() != 1 ||
      !render(processor, retained))
    return false;
  processor.resetParametersToDefaults(ParameterReset::parameters);
  return render(processor, 0.0F);
}

bool authoredDefault(Product product) {
  const auto source = juce::File(
      pathToJuce(testTemporaryRoot() / "unranged-parameter-default.onda"));
  if (!source.replaceWithText(program("test = 4.0")))
    return false;
  Processor processor(product);
  processor.loadFile(source);
  processor.prepareToPlay(1000.0, 8);
  if (!render(processor, 4.0F) || !roundTrip(processor, product, 4.0F))
    return false;
  processor.getParameters()[0]->setValueNotifyingHost(0.75F);
  if (!render(processor, 0.75F))
    return false;
  processor.resetParametersToDefaults(ParameterReset::parameters);
  if (!render(processor, 4.0F))
    return false;

  const auto previous = processor.workerStatus().engineGeneration;
  if (!source.replaceWithText(program("test = 0.5 {0.0, 1.0}")))
    return false;
  processor.requestReload();
  if (!test::waitUntil(
          [&] {
            const auto status = processor.workerStatus();
            return status.active && !status.compiling &&
                   status.engineGeneration != previous;
          },
          test::yieldToWorker) ||
      !render(processor, 1.0F))
    return false;
  const auto state =
      makeRunViewState(processor, processor.workerStatus(), true, {})["state"];
  const auto value = (*state["params"].getArray())[0]["value"];
  return (value.isVoid() || value == juce::var{1.0}) &&
         roundTrip(processor, product, 1.0F);
}

bool fullWidthIntegerDefault(const Product product, const char *literal) {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "full-width-integer-default.onda")};
  const auto program = [&](const juce::String &initial) {
    return "params { test: i64 = " + initial +
           "; following = 0.25 {0.0, 1.0} }\n"
           "outs { out }\n"
           "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
           "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
           "init { pinned = test }\n"
           "sample { if test == " + juce::String{literal} +
           " && pinned == " + juce::String{literal} +
           " { out = following } else { out = f32(test) + 2.0 } }\n";
  };
  if (!source.replaceWithText(program(literal)))
    return false;
  Processor processor(product);
  processor.loadFile(source);
  processor.prepareToPlay(1000.0, 8);
  test::service(processor);
  const auto status = processor.workerStatus();
  if (status.mappings.size() != std::min(slotCount, std::size_t{2}) ||
      status.mappings[0].name != "test" ||
      (slotCount > 1 && status.mappings[1].name != "following")) {
    std::cerr << "Full-width i64 default " << literal
              << " disappeared from the DAW slots\n";
    return false;
  }
  if (!render(processor, 0.25F) || !roundTrip(processor, product, 0.25F))
    return false;
  const auto view =
      makeRunViewState(processor, processor.workerStatus(), true, {})["state"];
  const auto &parameter = (*view["params"].getArray())[0];
  if (!parameter["value"].isString() ||
      parameter["value"].toString() != literal ||
      !parameter["default"].isString() ||
      parameter["default"].toString() != literal) {
    std::cerr << "Full-width i64 presentation lost " << literal << '\n';
    return false;
  }
  if constexpr (slotCount > 1) {
    processor.getParameters()[1]->setValue(0.75F);
    if (!render(processor, 0.75F))
      return false;
  }

  // Normalized automation replaces the authored integer with a 0/1 value.
  const std::array points{AutomationPoint{0, 1.0}};
  TestAutomationQueue queue{points};
  const auto id = juce::VST3ClientExtensions::convertJuceParameterId("slot01");
  if (!test::captureRealtime(processor, id, queue.queue(), 8) ||
      !render(processor, 3.0F) || !roundTrip(processor, product, 3.0F))
    return false;
  processor.setSlotValue(0, 0.0F);
  if (!render(processor, 2.0F))
    return false;
  processor.resetParametersToDefaults(ParameterReset::parameters);
  if (!render(processor, 0.25F) || !roundTrip(processor, product, 0.25F))
    return false;
  processor.requestUserReset();
  if (!render(processor, 0.25F))
    return false;

  // An unchanged domain retains the exact old default before initialization,
  // even when the reloaded declaration authors a different default.
  if (!source.replaceWithText(program("0")))
    return false;
  processor.requestReload();
  processor.prepareToPlay(1000.0, 8);
  if (!render(processor, 0.25F) || !roundTrip(processor, product, 0.25F) ||
      !source.deleteFile())
    return false;
  return roundTrip(processor, product, 0.25F);
}

bool validStateDefaults() {
  struct Case {
    ScalarDomain::Type type;
    ScalarPlain plain;
  };
  const Case invalid[]{
      {ScalarDomain::Type::i32, 2147483648.0},
      {ScalarDomain::Type::i32, 0.25},
      {ScalarDomain::Type::i32, std::int64_t{2147483648}},
      {ScalarDomain::Type::i64, std::ldexp(1.0, 63)},
      {ScalarDomain::Type::i64, -std::ldexp(1.0, 64)},
      {ScalarDomain::Type::f32, 1.0e100},
      {ScalarDomain::Type::f64, std::int64_t{9007199254740993}},
      {ScalarDomain::Type::boolean, 0.75},
  };
  for (const auto test : invalid) {
    ScalarDomain domain{0.0, 1.0};
    domain.type = test.type;
    domain.defaultMayExceedRange = true;
    ScalarValue value{1.0};
    value.scalarDefault = ScalarValue::ScalarDefault{test.plain, domain};
    juce::MemoryBlock bytes;
    juce::MemoryOutputStream output(bytes, false);
    writeScalarValue(output, value);
    juce::MemoryInputStream input(bytes, false);
    if (readScalarValue(input)) {
      std::cerr << "State accepted a default outside its scalar type\n";
      return false;
    }
  }
  return true;
}
} // namespace

bool exerciseUnrangedParameters() {
  if (!validStateDefaults())
    return false;
  for (const auto product : {Product::effect, Product::instrument}) {
    for (const auto *type : {"f32", "f64", "i32", "i64", "bool"})
      if (!check(product, type)) {
        std::cerr << "Unranged " << type << " controls failed for "
                  << (product == Product::effect ? "OndaFX" : "OndaSynth")
                  << '\n';
        return false;
      }
    if (!reload(product)) {
      std::cerr << "Unranged parameter automatic reload failed\n";
      return false;
    }
    if (!authoredDefault(product)) {
      std::cerr << "Unranged parameter authored default failed\n";
      return false;
    }
    for (const auto *literal : {"9007199254740993", "-9007199254740993",
                               "9223372036854775807", "-9223372036854775808"})
      if (!fullWidthIntegerDefault(product, literal))
        return false;
  }
  return true;
}
