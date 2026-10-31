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

bool exerciseGpuEditorSmoothingPublications() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "gpu-editor-smoothing.onda")};
  if (!source.replaceWithText(
          "event send(value: i64) {}\nsample { out1 = 0.0 }\n"))
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
  Editor editor(processor);
  window.addAndMakeVisible(editor);
  editor.setBounds(window.getLocalBounds());
  GpuView *view{};
  for (auto *child : editor.getChildren())
    if (auto *gpu = dynamic_cast<GpuView *>(child))
      view = gpu;
  if (!view || !test::waitForMessage([&] {
        EditorTestAccess::tick(editor);
        return !view->error().isEmpty() ||
               GpuViewTestAccess::initialized(*view);
      }) ||
      !view->error().isEmpty())
    return false;

  nativeMessage = GpuViewTestAccess::message(*view);
  juce::ScopedValueSetter<decltype(nativeMessage)> observe(
      GpuViewTestAccess::message(*view), observeMessage);
  const auto checkPatch = [&](const juce::String &error) {
    const auto state = messages.empty() ? juce::var{} : messages[0]["state"];
    if (messages.size() != 1 || messages[0]["type"].toString() != "state" ||
        !state.isObject() ||
        state.getDynamicObject()->getProperties().size() != 3 ||
        !state.hasProperty("paramSmoothingMs") ||
        !state.hasProperty("inputControlSmoothingMs") ||
        !test::withinTolerance(
            static_cast<double>(state["paramSmoothingMs"]) -
                processor.paramSmoothingSeconds() * 1000.0,
            1.0e-9) ||
        !test::withinTolerance(
            static_cast<double>(state["inputControlSmoothingMs"]) -
                processor.inputControlSmoothingSeconds() * 1000.0,
            1.0e-9) ||
        state["error"].toString() != error) {
      std::cerr << "Expected only accepted smoothing values and error, received "
                << messages.size() << " messages: "
                << juce::JSON::toString(state) << '\n';
      return false;
    }
    return true;
  };
  const auto command = [&](const char *type, double milliseconds) {
    messages.clear();
    juce::var input{new juce::DynamicObject};
    input.getDynamicObject()->setProperty("type", type);
    input.getDynamicObject()->setProperty("milliseconds", milliseconds);
    EditorTestAccess::command(editor, input);
    // The next timer tick must not repeat the change as a full schema update.
    EditorTestAccess::tick(editor);
  };
  for (const auto *type : {"setParamSmoothing", "setInputControlSmoothing"}) {
    for (const auto milliseconds : {42.0, -1.0, 42.0, 0.0}) {
      command(type, milliseconds);
      const auto error = messages.empty() ? juce::String{}
                                         : messages[0]["state"]["error"].toString();
      if (error.isNotEmpty() != (milliseconds < 0.0) || !checkPatch(error))
        return false;
    }
  }

  // Changes made outside the editor use the same bounded timer publication.
  if (!processor.setParamSmoothingSeconds(0.012) ||
      !processor.setInputControlSmoothingSeconds(0.034))
    return false;
  messages.clear();
  EditorTestAccess::tick(editor);
  if (!checkPatch({}))
    return false;

  if (!source.replaceWithText("this is not valid Onda\n"))
    return false;
  processor.loadFile(source);
  processor.prepareToPlay(48'000.0, 64);
  const auto status = processor.workerStatus();
  if (!status.hasError)
    return false;
  EditorTestAccess::tick(editor);
  command("setParamSmoothing", -1.0);
  command("setParamSmoothing", 12.0);
  return checkPatch(juce::String(status.message));
}

bool exerciseGpuEditorFileDrag() {
  using namespace onda::plugin;
  Processor processor(Product::effect);
  processor.setNonRealtime(true);
  processor.prepareToPlay(48'000.0, 64);
  juce::Component window;
  window.setSize(720, 840);
  window.addToDesktop(0);
  window.setVisible(true);
  Editor editor(processor);
  window.addAndMakeVisible(editor);
  editor.setBounds(window.getLocalBounds());
  GpuView *view{};
  for (auto *child : editor.getChildren())
    if (auto *gpu = dynamic_cast<GpuView *>(child))
      view = gpu;
  if (!view || !test::waitForMessage([&] {
        EditorTestAccess::tick(editor);
        return !view->error().isEmpty() ||
               GpuViewTestAccess::initialized(*view);
      }) ||
      !view->error().isEmpty())
    return false;
  nativeMessage = GpuViewTestAccess::message(*view);
  juce::ScopedValueSetter<decltype(nativeMessage)> observe(
      GpuViewTestAccess::message(*view), observeMessage);
  const auto hasDrag = [](const char *type, const char *phase) {
    return !messages.empty() && messages.back()["type"].toString() == type &&
           messages.back()["phase"].toString() == phase;
  };
  for (const auto *extension : {"onda", "on", "ondaproject"}) {
    const auto path = pathToJuce(testTemporaryRoot() /
                                 (std::string("drag-source.") + extension));
    const juce::StringArray files{path};
    if (!editor.isInterestedInFileDrag(files))
      return false;
    messages.clear();
    editor.fileDragEnter(files, 360, 420);
    if (!hasDrag("sourceFileDrag", "enter")) {
      std::cerr << "Source drag did not reach the source-picker highlight\n";
      return false;
    }
    editor.fileDragMove(files, 361, 421);
    if (!hasDrag("sourceFileDrag", "over") ||
        static_cast<int>(messages.back()["position"]["x"]) != 361 ||
        messages.back()["paths"][0].toString() != path)
      return false;
    editor.fileDragExit(files);
    if (!hasDrag("sourceFileDrag", "leave"))
      return false;
  }
  const juce::File patch{pathToJuce(testTemporaryRoot() / "drag-buffer.onda")};
  if (!patch.replaceWithText(
          "buffers { clip: buffer<f32> }\nsample { out1 = clip[0] }\n"))
    return false;
  processor.loadFile(patch);
  processor.prepareToPlay(48'000.0, 64);
  if (!processor.workerStatus().active) {
    std::cerr << "Buffer drag fixture failed: "
              << processor.workerStatus().message << '\n';
    return false;
  }
  EditorTestAccess::tick(editor);
  const juce::StringArray audio{
      pathToJuce(testTemporaryRoot() / "drag-clip.wav")};
  if (!editor.isInterestedInFileDrag(audio)) {
    std::cerr << "Loaded buffer did not accept an audio drag\n";
    return false;
  }
  editor.fileDragEnter(audio, 100, 200);
  return hasDrag("bufferFileDrag", "enter");
}

bool exerciseGpuEditorRestoreBeforePreparation() {
  using namespace onda::plugin;
  const juce::File source{pathToJuce(testTemporaryRoot() / "gpu-restore.onda")};
  juce::String code{"params:\n"};
  for (int i = 0; i < 32; ++i)
    code += "  p" + juce::String(i) + " = 0.0\n";
  code += "event send(value: i64) {}\nsample { out1 = 0.0 }\n";
  if (!source.replaceWithText(code))
    return false;
  Processor original(Product::effect);
  original.loadFile(source);
  auto saved = juce::JSON::parse(R"({"scrollTop":300,"eventValues":[{
    "name":"send","args":[{"name":"value","shape":{"kind":"scalar","encoding":"i64"},"default":"0"}],"values":["77"]
  }]})");
  saved.getDynamicObject()->setProperty("path", source.getFullPathName());
  original.setViewState(saved);
  juce::MemoryBlock state;
  original.getStateInformation(state);
  Processor restored(Product::effect);
  restored.setNonRealtime(true);
  restored.setStateInformation(state.getData(),
                               static_cast<int>(state.getSize()));

  juce::Component window;
  window.setSize(720, 480);
  window.addToDesktop(0);
  window.setVisible(true);
  Editor editor(restored);
  window.addAndMakeVisible(editor);
  editor.setBounds(window.getLocalBounds());
  GpuView *view{};
  for (auto *child : editor.getChildren())
    if (auto *gpu = dynamic_cast<GpuView *>(child))
      view = gpu;
  if (!view || !test::waitForMessage([&] {
        EditorTestAccess::tick(editor);
        return !view->error().isEmpty() ||
               GpuViewTestAccess::initialized(*view);
      }) ||
      !view->error().isEmpty())
    return false;

  restored.prepareToPlay(48'000.0, 64);
  if (!restored.workerStatus().active)
    return false;
  EditorTestAccess::tick(editor);
  GpuViewTestAccess::sendInput(*view,
                               R"({"type":"pointerMove","x":300,"y":300})");
  GpuViewTestAccess::sendInput(*view, R"({"type":"scroll","delta":1})");
  GpuViewTestAccess::sendInput(*view, R"({"type":"focusLost"})");
  const auto presentation = juce::JSON::parse(restored.viewState());
  if (presentation["eventValues"][0]["values"][0].toString() != "77" ||
      static_cast<double>(presentation["scrollTop"]) < 300.0) {
    std::cerr << "Early editor restoration lost its saved presentation: "
              << juce::JSON::toString(presentation) << '\n';
    return false;
  }
  return true;
}

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
  auto saved = juce::JSON::parse(R"({"scopePinned":true,"sections":{"scope":false}})");
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
        restored["scopePinned"] != saved["scopePinned"] ||
        nextReadyId <= readyId || scopeCalls != 0 ||
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
          return !view->error().isEmpty() || view->meterVisible();
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
