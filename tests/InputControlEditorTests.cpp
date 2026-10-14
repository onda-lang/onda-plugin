#include "InputControlEditorTests.h"
#include "ControlInputTestSupport.h"
#include "EditorTestSupport.h"
#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "RunViewHost.h"
#include "TemporaryDirectory.h"
#include "TestAutomationQueue.h"

using namespace onda::plugin;

namespace {
juce::var command(const char *type, const char *name = nullptr,
                  double value = 0.0) {
  auto *object = new juce::DynamicObject;
  object->setProperty("type", type);
  if (name != nullptr) {
    object->setProperty("name", name);
    object->setProperty("value", value);
    object->setProperty("commit", true);
  }
  return juce::var{object};
}

bool exercise(Product product) {
  const auto source =
      juce::File(pathToJuce(testTemporaryRoot() / "input-control-editor.onda"));
  const auto text =
      "ins {\n" + test::mainAudioInputDeclaration() +
      " cv = 0.0 {-2.0, 2.0}\n}\nouts { out1 }\n"
      "params { level = 0.25 {0.0, 1.0} }\n"
      "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
      "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
      "sample { out1 = cv }\n";
  if (!source.replaceWithText(text))
    return false;
  Processor processor(product);
  processor.prepareToPlay(1000.0, 8);
  if (!processor.setPreferredBlockSize(3) ||
      !processor.setInputControlSmoothingSeconds(0.01))
    return false;
  processor.loadFile(source);
  processor.prepareToPlay(1000.0, 8);
  const auto render = [&](int frames, auto expected, bool split = false) {
    juce::AudioBuffer<float> audio(
        std::max(processor.getTotalNumInputChannels(),
                 processor.getTotalNumOutputChannels()),
        frames);
    audio.clear();
    juce::MidiBuffer midi;
    if (split)
      midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), 2);
    if (!test::processRealtime(processor, audio, midi))
      return false;
    for (int frame = 0; frame < frames; ++frame) {
      if (!test::withinTolerance(audio.getSample(0, frame) - expected(frame),
                                 2.0e-6F)) {
        std::cerr << "Input control editor sample " << frame << ": "
                  << audio.getSample(0, frame) << " expected "
                  << expected(frame) << '\n';
        return false;
      }
    }
    return true;
  };
  if (!render(8, [](int) { return 0.0F; }))
    return false;
  Editor editor(processor);
  EditorTestAccess::command(editor,
                            command("setInputControl", "Input 1 -> cv", 1.0));
  if (!render(0, [](int) { return 0.0F; }) ||
      !render(
          8, [](int frame) { return 0.2F * static_cast<float>(frame + 1); },
          true) ||
      !render(3, [](int frame) {
        return std::min(2.0F, 1.8F + 0.2F * static_cast<float>(frame));
      }))
    return false;
  EditorTestAccess::command(editor,
                            command("setInputControl", "Input 1 -> cv", 0.0));
  if (!render(3, [](int frame) {
        return 2.0F - 0.4F * static_cast<float>(frame + 1);
      }))
    return false;
  EditorTestAccess::command(editor,
                            command("setInputControl", "Input 1 -> cv", 0.75));
  if (!render(3, [](int frame) {
        return 0.8F + 0.02F * static_cast<float>(frame + 1);
      }))
    return false;

  const auto id = juce::VST3ClientExtensions::convertJuceParameterId("input01");
  const std::array points{AutomationPoint{0, 0.25}};
  TestAutomationQueue queue{points};
  if (!test::captureRealtime(processor, id, queue.queue(), 8) ||
      !render(8, [](int) { return -1.0F; }))
    return false;
  // A later editor change starts from the final sample written by automation.
  processor.setInputControlValue(0, 0.75F);
  if (!render(3, [](int frame) {
        return -1.0F + 0.2F * static_cast<float>(frame + 1);
      }))
    return false;

  // A newer editor write must cancel a published curve and ramp from the
  // previous rendered value, even before the curve renders its first sample.
  const std::array pendingPoints{AutomationPoint{7, 0.9}};
  TestAutomationQueue pending{pendingPoints};
  for (const auto split : {false, true}) {
    processor.getParameters()[static_cast<int>(slotCount)]->setValue(0.5F);
    if (!render(8, [](int) { return 0.0F; }) ||
        !test::captureRealtime(processor, id, pending.queue(), 8) ||
        !test::auditRealtime(
            [&] { processor.publishHostAutomationTargets(); }))
      return false;
    processor.setInputControlValue(0, 0.6F);
    if (!render(
            8, [](int frame) { return 0.04F * static_cast<float>(frame + 1); },
            split) ||
        !render(2, [](int frame) {
          return 0.36F + 0.04F * static_cast<float>(frame);
        }))
      return false;
  }

  processor.setSlotValue(0, 0.9F);
  EditorTestAccess::command(editor, command("resetInputControls"));
  if (!std::is_eq(processor.slotValue(0) <=> 0.9F) ||
      !render(8, [](int) { return 0.0F; }))
    return false;
  processor.setInputControlValue(0, 0.8F);
  EditorTestAccess::command(editor, command("resetParams"));
  if (!std::is_eq(processor.inputControlValue(0) <=> 0.8F) ||
      !std::is_eq(processor.slotValue(0) <=> 0.25F))
    return false;

  auto layout = processor.getBusesLayout();
  layout.inputBuses.set(pluginMainInputBuses, juce::AudioChannelSet::mono());
  if (!processor.setBusesLayout(layout))
    return false;
  juce::AudioBuffer<float> external(
      std::max(processor.getTotalNumInputChannels(),
               processor.getTotalNumOutputChannels()),
      8);
  external.clear();
  std::fill_n(external.getWritePointer(pluginInputChannels), 8, 0.33F);
  juce::MidiBuffer midi;
  if (!test::processRealtime(processor, external, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(external.getSample(0, frame) - 0.33F, 1.0e-6F))
      return false;
  EditorTestAccess::command(editor,
                            command("setInputControl", "Input 1 -> cv", 0.0));
  if (!std::is_eq(processor.inputControlValue(0) <=> 0.8F))
    return false;
  auto state = makeRunViewState(processor, processor.workerStatus(), false, {},
                                false, false)
                   .getDynamicObject()
                   ->getProperty("state");
  if (state.getDynamicObject()
          ->getProperty("inputControls")
          .getArray()
          ->getReference(0)
          .getDynamicObject()
          ->getProperty("enabled") != juce::var{false})
    return false;
  layout.inputBuses.set(pluginMainInputBuses,
                        juce::AudioChannelSet::disabled());
  if (!processor.setBusesLayout(layout) || !render(8, [](int) { return 1.2F; }))
    return false;

  processor.setInputControlLayout(ParamControlLayout::sliders);
  juce::MemoryBlock saved;
  processor.getStateInformation(saved);
  Processor restored(product);
  restored.setStateInformation(saved.getData(),
                               static_cast<int>(saved.getSize()));
  if (!std::is_eq(restored.inputControlSmoothingSeconds() <=> 0.01) ||
      restored.inputControlLayout() != ParamControlLayout::sliders ||
      !std::is_eq(restored.paramSmoothingSeconds() <=>
                  defaultParamSmoothingSeconds))
    return false;
  // An identical normalized editor write still replaces an exact plain default.
  const auto wide = text.replace(
      "cv = 0.0 {-2.0, 2.0}", "cv = 0.25 {-1000000000000000000000000000000.0, "
                              "1000000000000000000000000000000.0}");
  const auto wideSource = juce::File(
      pathToJuce(testTemporaryRoot() / "input-control-wide-default.onda"));
  if (!wideSource.replaceWithText(wide))
    return false;
  processor.loadFile(wideSource);
  processor.prepareToPlay(1000.0, 8);
  if (!render(8, [](int) { return 0.25F; })) {
    std::cerr << "Wide control preparation: "
              << processor.workerStatus().message << '\n';
    return false;
  }
  auto wideState = makeRunViewState(processor, processor.workerStatus(), false,
                                    {}, false, false)
                       .getDynamicObject()
                       ->getProperty("state");
  const auto &wideControl = wideState.getDynamicObject()
                                ->getProperty("inputControls")
                                .getArray()
                                ->getReference(0);
  if (!std::is_eq(static_cast<double>(
                      wideControl.getDynamicObject()->getProperty("value")) <=>
                  0.25))
    return false;
  processor.setInputControlValue(0, 0.5F);
  const std::array feedback{AutomationPoint{0, 0.5}};
  TestAutomationQueue echo{feedback};
  if (!test::captureRealtime(processor, id, echo.queue(), 8) ||
      !render(8, [](int frame) {
        return 0.25F - 0.025F * static_cast<float>(frame + 1);
      }))
    return false;
  return true;
}
} // namespace

bool exerciseInputControlEditor() {
  if constexpr (controlCount == 0)
    return true;
  return exercise(Product::effect) && exercise(Product::instrument);
}
