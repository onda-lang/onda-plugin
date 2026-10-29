#include "NativeEditorTests.h"

#include "EditorTestSupport.h"
#include "GpuView.h"
#include "GpuViewTestSupport.h"
#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace {
decltype(&onda_ui_input) nativeInput{};
int inputCalls{};
int countInput(OndaUi *ui, const char *json) {
  ++inputCalls;
  return nativeInput(ui, json);
}
decltype(&onda_ui_message) nativeMessage{};
decltype(&onda_ui_scope) nativeScope{};
std::vector<juce::var> messages;
int scopeCalls{};
int observeMessage(OndaUi *ui, const char *json) {
  messages.push_back(juce::JSON::parse(juce::String::fromUTF8(json)));
  return nativeMessage(ui, json);
}
void countScope(OndaUi *ui, uint32_t channels, const float *samples,
                size_t count) {
  ++scopeCalls;
  nativeScope(ui, channels, samples, count);
}
} // namespace

bool exerciseGpuEditorPeerRecreation() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "gpu-editor-peer.onda")};
  if (!source.replaceWithText(
          "params { gain = 0.25 {0.0, 1.0} }\nsample { out1 = gain }\n"))
    return false;
  Processor processor(Product::effect);
  processor.setNonRealtime(true);
  processor.loadFile(source);
  processor.prepareToPlay(48'000.0, 64);
  if (!processor.workerStatus().active)
    return false;
  auto saved = juce::JSON::parse(R"({"sections":{"scope":false}})");
  saved.getDynamicObject()->setProperty("path", source.getFullPathName());
  processor.setViewState(saved);

  juce::Component first, second;
  for (auto *window : {&first, &second}) {
    window->setSize(720, 840);
    window->addToDesktop(0);
    window->setVisible(true);
  }
  Editor editor(processor);
  first.addAndMakeVisible(editor);
  editor.setBounds(first.getLocalBounds());
  GpuView *view{};
  for (auto *child : editor.getChildren())
    if (auto *gpu = dynamic_cast<GpuView *>(child))
      view = gpu;
  const auto *overlay = dynamic_cast<juce::Label *>(
      editor.findChildWithID("onda-loading-overlay"));
  if (!view || !overlay || !test::waitForMessage([&] {
        EditorTestAccess::tick(editor);
        return !view->error().isEmpty() || !overlay->isVisible();
      }) ||
      !view->error().isEmpty() || view->scopeVisible())
    return false;

  nativeMessage = GpuViewTestAccess::message(*view);
  nativeScope = GpuViewTestAccess::scope(*view);
  juce::ScopedValueSetter<decltype(nativeMessage)> observe(
      GpuViewTestAccess::message(*view), observeMessage);
  juce::ScopedValueSetter<decltype(nativeScope)> count(
      GpuViewTestAccess::scope(*view), countScope);
  int readyId{};
  // Keep both peers visible so hidden-editor resets cannot mask lost state.
  for (auto *window : {&second, &first}) {
    messages.clear();
    scopeCalls = 0;
    window->addAndMakeVisible(editor);
    EditorTestAccess::tick(editor);
    if (!view->error().isEmpty() || view->scopeVisible()) {
      std::cerr << "Recreated GPU editor lost its collapsed scope: "
                << view->error() << '\n';
      return false;
    }
    const auto state = std::find_if(
        messages.begin(), messages.end(), [](const auto &message) {
          return message["type"].toString() == "state" &&
                 message["state"]["viewState"].isObject();
        });
    if (state == messages.end()) {
      std::cerr << "Recreated GPU editor received no presentation restore\n";
      return false;
    }
    const auto restored = (*state)["state"]["viewState"];
    const auto nextReadyId = static_cast<int>(restored["readyId"]);
    if (restored["path"] != saved["path"] ||
        restored["sections"]["scope"] != saved["sections"]["scope"] ||
        nextReadyId <= readyId || scopeCalls == 0 ||
        std::none_of(messages.begin(), messages.end(), [](const auto &message) {
          return message["type"].toString() == "midiActivity";
        })) {
      std::cerr << "Incomplete GPU peer restoration: "
                << juce::JSON::toString(restored) << ", scope calls "
                << scopeCalls << ", messages " << messages.size() << '\n';
      return false;
    }
    readyId = nextReadyId;
  }
  return true;
}

bool exerciseGpuEditorTeardown() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "gpu-editor-teardown.onda")};
  if (!source.replaceWithText(
          "params { gain = 0.25 {0.0, 1.0} }\nsample { out1 = gain }\n"))
    return false;
  Processor processor(Product::effect);
  processor.setNonRealtime(true);
  processor.loadFile(source);
  processor.prepareToPlay(48'000.0, 64);
  if (!processor.workerStatus().active)
    return false;

  juce::Component window;
  window.setSize(720, 840);
  window.addToDesktop(0);
  window.setVisible(true);
  for (const auto previousError : {false, true}) {
    if (!processor.setParamSmoothingSeconds(0.03))
      return false;
    auto editor = std::make_unique<Editor>(processor);
    window.addAndMakeVisible(*editor);
    editor->setBounds(window.getLocalBounds());
    GpuView *view{};
    for (auto *child : editor->getChildren())
      if (auto *gpu = dynamic_cast<GpuView *>(child))
        view = gpu;
    if (!view || !test::waitForMessage([&] {
          view->tick();
          return !view->error().isEmpty() || view->scopeVisible();
        }) ||
        !view->error().isEmpty())
      return false;

    {
      nativeInput = GpuViewTestAccess::input(*view);
      juce::ScopedValueSetter<decltype(nativeInput)> observe(
          GpuViewTestAccess::input(*view), countInput);
      inputCalls = 0;
      window.setVisible(false);
      EditorTestAccess::tick(*editor);
      const auto suspendedCalls = inputCalls;
      if (suspendedCalls == 0)
        return false;
      for (int tick = 0; tick < 60; ++tick)
        EditorTestAccess::tick(*editor);
      if (inputCalls != suspendedCalls)
        return false;
      window.setVisible(true);
      EditorTestAccess::tick(*editor);
      const auto resumedCalls = inputCalls;
      window.setVisible(false);
      EditorTestAccess::tick(*editor);
      if (inputCalls == resumedCalls)
        return false;
      window.setVisible(true);
      EditorTestAccess::tick(*editor);
    }

    if (previousError)
      EditorTestAccess::command(
          *editor, juce::JSON::parse(
                       R"({"type":"setParamSmoothing","milliseconds":-1})"));
    bool editingSmoothing{};
    for (int stop = 0; stop < 32 && !editingSmoothing; ++stop) {
      view->keyPressed(juce::KeyPress(juce::KeyPress::tabKey));
      editingSmoothing = view->isTextInputActive() &&
                         test::withinTolerance(
                             view->getTextInRange({0, view->getTotalNumChars()})
                                     .getDoubleValue() -
                                 30.0,
                             1.0e-9);
    }
    if (!editingSmoothing)
      return false;
    view->insertTextAtCaret("47");
    // Destroy the editor while still attached, without a prior focus-loss
    // event. Teardown must commit the draft before its callback dependencies
    // disappear.
    editor.reset();
    if (!test::withinTolerance(processor.paramSmoothingSeconds() - 0.047,
                               1.0e-9))
      return false;
  }
  return true;
}
