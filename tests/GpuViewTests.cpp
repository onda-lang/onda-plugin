#include "GpuView.h"
#include "GpuViewTestSupport.h"
#include <iostream>
#include <juce_gui_basics/juce_gui_basics.h>
#include <stdexcept>

namespace {
void check(bool success, const char *message) {
  if (!success)
    throw std::runtime_error(message);
}
std::vector<juce::var> keyboardInputs;
int keyboardResult{};
int recordInput(OndaUi *, const char *json) {
  keyboardInputs.push_back(juce::JSON::parse(juce::String::fromUTF8(json)));
  return keyboardResult;
}
void exerciseKeyboardRouting() {
  onda::plugin::GpuView view([](const auto &) {}, [] {});
  check(!view.keyStateChanged(true) && !view.keyStateChanged(false),
        "Unused key state changes were swallowed");
  check(!view.keyPressed(juce::KeyPress(' ', {}, ' ')),
        "Space was swallowed before the UI initialized");
  onda::plugin::GpuViewTestAccess::connectInput(view, recordInput);
  for (const auto &key :
       {juce::KeyPress(' ', {}, ' '), juce::KeyPress('A', {}, 'a'),
        juce::KeyPress(juce::KeyPress::returnKey),
        juce::KeyPress(juce::KeyPress::leftKey),
        juce::KeyPress('C', juce::ModifierKeys::commandModifier, 'c')}) {
    for (const int consumed : {0, 1}) {
      keyboardInputs.clear();
      keyboardResult = consumed;
      check(view.keyPressed(key) == (consumed == 1),
            "Key handling did not reflect the shared UI's consumption");
      check(keyboardInputs.size() == 1 &&
                keyboardInputs[0]["type"].toString() != "computerKey",
            "Plugin generated computer MIDI outside DAW routing");
      check(view.error().isEmpty(), "Unused key was treated as an input error");
    }
  }
  keyboardInputs.clear();
  check(!view.keyPressed(juce::KeyPress(juce::KeyPress::F1Key)) &&
            !view.keyPressed(juce::KeyPress(
                'V', juce::ModifierKeys::commandModifier, 'v')) &&
            !view.keyPressed(juce::KeyPress(juce::KeyPress::returnKey,
                                            juce::ModifierKeys::commandModifier,
                                            0)) &&
            keyboardInputs.empty(),
        "Unused host shortcut was swallowed");
  keyboardResult = -1;
  check(!view.keyPressed(juce::KeyPress(' ', {}, ' ')) &&
            view.error() == "input error",
        "Input errors were lost or reported as consumed keys");
}
struct Window final : juce::Component {
  Window() { setOpaque(true); }
  void paint(juce::Graphics &graphics) override {
    graphics.fillAll(juce::Colours::magenta);
  }
};
void pump(onda::plugin::GpuView &view) {
  juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
  view.tick();
  check(view.error().isEmpty(), view.error().toRawUTF8());
}
#if JUCE_WINDOWS
void checkWindowsRendererOwnership() {
  Window first, second;
  for (auto *window : {&first, &second}) {
    window->setSize(720, 840);
    window->addToDesktop(0);
    window->setVisible(true);
  }
  const auto engines = first.getPeer()->getAvailableRenderingEngines();
  const auto direct2D = engines.indexOf("Direct2D");
  const auto software = engines.indexOf("Software Renderer");
  check(direct2D >= 0 && software >= 0, "Missing Windows rendering engines");
  first.getPeer()->setCurrentRenderingEngine(direct2D);
  second.getPeer()->setCurrentRenderingEngine(direct2D);
  {
    int ready = 0;
    onda::plugin::GpuView view([](const auto &) {}, [&] { ++ready; });
    first.addAndMakeVisible(view);
    view.setBounds(first.getLocalBounds());
    pump(view);
    check(ready == 1 &&
              first.getPeer()->getCurrentRenderingEngine() == software,
          "JUCE composition still covers the GPU surface");
    first.repaint();
    pump(view);
    second.addAndMakeVisible(view);
    pump(view);
    check(ready == 2 &&
              first.getPeer()->getCurrentRenderingEngine() == direct2D &&
              second.getPeer()->getCurrentRenderingEngine() == software,
          "Reparenting did not transfer GPU presentation ownership");
  }
  check(second.getPeer()->getCurrentRenderingEngine() == direct2D,
        "Closing the GPU view did not restore the previous renderer");
  first.getPeer()->setCurrentRenderingEngine(software);
  {
    onda::plugin::GpuView view([](const auto &) {}, [] {});
    first.addAndMakeVisible(view);
    view.setBounds(first.getLocalBounds());
    pump(view);
  }
  check(first.getPeer()->getCurrentRenderingEngine() == software,
        "Closing the GPU view changed an existing software renderer");
  {
    onda::plugin::GpuView view([](const auto &) {}, [] {});
    second.addAndMakeVisible(view);
    view.setBounds(second.getLocalBounds());
    pump(view);
    second.removeFromDesktop();
    pump(view);
  }
  check(first.getPeer()->getCurrentRenderingEngine() == software,
        "Releasing a destroyed peer changed another window's renderer");
}
#endif
juce::MouseEvent mouse(juce::Component &view, juce::Point<float> position,
                       int modifiers = juce::ModifierKeys::leftButtonModifier) {
  const auto now = juce::Time::getCurrentTime();
  return {juce::Desktop::getInstance().getMainMouseSource(),
          position,
          juce::ModifierKeys(modifiers),
          1.0f,
          0.0f,
          0.0f,
          0.0f,
          0.0f,
          &view,
          &view,
          now,
          position,
          now,
          1,
          false};
}
void exercise() {
  std::vector<juce::var> commands;
  bool ready = false;
  Window window;
  onda::plugin::GpuView view(
      [&](const auto &command) { commands.push_back(command); },
      [&] { ready = true; });
  window.setSize(720, 840);
  window.addAndMakeVisible(view);
  view.setBounds(window.getLocalBounds());
  window.addToDesktop(0);
  window.setVisible(true);
  for (int attempt = 0; attempt < 100 && !ready; ++attempt)
    pump(view);
  check(ready, "GPU view did not initialize its native surface");
#if JUCE_MAC
  const auto *display =
      juce::Desktop::getInstance().getDisplays().getDisplayForRect(
          view.getScreenBounds());
  check(display && juce::approximatelyEqual(
                       onda::plugin::GpuViewTestAccess::scale(view),
                       static_cast<float>(display->scale)),
        "GPU framebuffer scale did not match the native display density");
#endif
  view.message(juce::JSON::parse(
      R"({"type":"state","state":{"path":"keyboard.onda","running":true,"supportsScope":false,"supportsTransport":false,"midi":{"noteOn":true,"noteOff":true},"midiKeyboardInteractive":true,"showComputerKeyLabels":false,"currentMidiInputDevice":null}})"));
  const juce::KeyPress pianoKey{'A', {}, 'a'};
  commands.clear();
  check(!view.keyPressed(pianoKey) && !view.keyStateChanged(false) &&
            !view.keyPressed(juce::KeyPress(' ', {}, ' ')) &&
            !view.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)),
        "Idle instrument swallowed DAW keyboard input");
  check(commands.empty(), "Computer key bypassed DAW MIDI routing");
  view.message(juce::JSON::parse(
      R"({"type":"state","state":{"path":"native.onda","running":true,"supportsScope":false,"paramLayout":"knobs",
    "params":[{"name":"Slot 1 -> frequency","type":"f64","value":440,"default":220,
      "rangeMin":20,"rangeMax":20000,"scale":"log"}]}})"));
  pump(view);
  commands.clear();
  view.mouseDown(
      mouse(view, {102.0f, 253.0f}, juce::ModifierKeys::rightButtonModifier));
  view.mouseDrag(
      mouse(view, {154.0f, 253.0f}, juce::ModifierKeys::rightButtonModifier));
  view.mouseUp(
      mouse(view, {154.0f, 253.0f}, juce::ModifierKeys::rightButtonModifier));
  check(commands.empty(), "Right-click changed a parameter or started editing");
  view.keyPressed(juce::KeyPress('8', {}, '8'));
  view.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
  check(std::none_of(commands.begin(), commands.end(),
                     [](const auto &command) {
                       return command["type"].toString() == "setParam";
                     }),
        "Right-click entered a numeric draft");
  commands.clear();
  view.mouseMove(mouse(view, {102.0f, 195.0f}));
  check(view.getMouseCursor() == juce::MouseCursor::UpDownResizeCursor,
        "Knob hover did not update the native cursor");
  view.mouseMove(mouse(view, {102.0f, 253.0f}));
  check(view.getMouseCursor() == juce::MouseCursor::LeftRightResizeCursor,
        "Number hover did not expose horizontal dragging");
  view.mouseExit(mouse(view, {102.0f, 253.0f}));
  check(view.getMouseCursor() == juce::MouseCursor::NormalCursor,
        "Leaving the view retained a control cursor");
  view.mouseDown(mouse(view, {102.0f, 253.0f}));
  view.mouseUp(mouse(view, {102.0f, 253.0f}));
  for (const char key : std::string("880"))
    view.keyPressed(
        juce::KeyPress(key, {}, static_cast<juce::juce_wchar>(key)));
  view.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
  commands.erase(std::remove_if(commands.begin(), commands.end(),
                                [](const auto &c) {
                                  return c["type"].toString() == "viewState";
                                }),
                 commands.end());
  check(commands.size() == 1,
        "Numeric editing did not emit one committed command");
  check(commands[0]["type"].toString() == "setParam",
        "Numeric editing emitted the wrong command");
  check(static_cast<double>(commands[0]["value"]) == 880.0,
        "Numeric editing lost the entered value");
  check(static_cast<bool>(commands[0]["commit"]),
        "Numeric editing left a DAW gesture open");
  commands.clear();
  view.mouseDown(mouse(view, {102.0f, 253.0f}));
  view.mouseUp(mouse(view, {102.0f, 253.0f}));
  check(view.isTextInputActive(), "Native text input did not activate");
  view.insertTextAtCaret("123");
  view.setTemporaryUnderlining({juce::Range<int>{0, 3}});
  view.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
  check(view.isTextInputActive() && view.getTextInRange({0, 3}) == "123",
        "Enter committed text while the input method was composing");
  check(std::none_of(commands.begin(), commands.end(),
                     [](const auto &command) {
                       return command["type"].toString() == "setParam";
                     }),
        "Composition published an uncommitted parameter");
  view.setTemporaryUnderlining({});
  view.setHighlightedRegion({1, 2});
  check(view.getTextInRange(view.getHighlightedRegion()) == "2",
        "Native text selection lost its range");
  view.insertTextAtCaret("4");
  check(view.getTextInRange({0, view.getTotalNumChars()}) == "143" &&
            view.getCaretPosition() == 2,
        "Native text insertion did not replace the selected substring");
  pump(view);
  const auto caret = view.getCaretRectangleForCharIndex(2);
  check(!caret.isEmpty() && view.getCharIndexForPoint(caret.getCentre()) == 2,
        "Native caret geometry disagreed with shaped text");
  check(view.getTextBounds({0, 3}).getBounds().getWidth() > caret.getWidth(),
        "Native text bounds lost the selected width");
  view.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
  check(!view.isTextInputActive(), "Escape left native text input active");
  view.mouseDown(mouse(view, {102.0f, 253.0f}));
  view.mouseUp(mouse(view, {102.0f, 253.0f}));
  view.keyPressed(juce::KeyPress(juce::KeyPress::homeKey));
  view.keyPressed(juce::KeyPress(juce::KeyPress::rightKey,
                                 juce::ModifierKeys::shiftModifier, 0));
  view.keyPressed(
      juce::KeyPress('C', juce::ModifierKeys::commandModifier, 'c'));
  check(juce::SystemClipboard::getTextFromClipboard() == "8",
        "Copy lost the selected substring");
  view.keyPressed(
      juce::KeyPress('X', juce::ModifierKeys::commandModifier, 'x'));
  check(juce::SystemClipboard::getTextFromClipboard() == "8",
        "Cut lost the selected substring");
  view.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
  commands.clear();
  view.mouseDown(mouse(view, {102.0f, 253.0f}));
  view.mouseDrag(mouse(view, {154.0f, 253.0f}));
  view.mouseUp(mouse(view, {154.0f, 253.0f}));
  check(std::any_of(commands.begin(), commands.end(),
                    [](const auto &command) {
                      return command["type"].toString() == "setParam" &&
                             static_cast<bool>(command["commit"]) &&
                             static_cast<double>(command["value"]) > 880.0;
                    }),
        "Horizontal number dragging did not commit to the host");
  const std::vector<float> samples(2048, 0.25f);
  for (int frame = 0; frame < 10; ++frame) {
    view.scope(2, samples);
    pump(view);
  }
  for (const char *panel : {"params", "inputControls", "buffers", "events",
                            "log", "midi", "settings"}) {
    auto state =
        juce::JSON::parse(juce::File(ONDA_GPU_UI_FIXTURE).loadFileAsString());
    check(state.isObject(), "Missing complete panel fixture");
    state.getDynamicObject()->setProperty("path",
                                          juce::String(panel) + ".onda");
    juce::var presentation{new juce::DynamicObject};
    presentation.getDynamicObject()->setProperty("activePanel", panel);
    state.getDynamicObject()->setProperty("viewState", presentation);
    juce::var message{new juce::DynamicObject};
    message.getDynamicObject()->setProperty("type", "state");
    message.getDynamicObject()->setProperty("state", state);
    view.message(message);
    view.scope(2, samples);
    pump(view);
    check(view.scopeVisible(),
          "Expanded scope did not request capture through the C interface");
  }
  view.message(juce::JSON::parse(
      R"({"type":"state","state":{"path":"collapsed.onda","viewState":{"sections":{"scope":false}}}})"));
  pump(view);
  check(!view.scopeVisible(), "Collapsed scope continued requesting capture");
  window.setSize(960, 720);
  view.setBounds(window.getLocalBounds());
  pump(view);
  window.setVisible(false);
  pump(view);
  window.setVisible(true);
  pump(view);
}
} // namespace
int main(int argc, char **argv) {
  try {
    juce::ScopedJuceInitialiser_GUI gui;
    exerciseKeyboardRouting();
    if (argc == 2 && juce::String(argv[1]) == "--keyboard") {
      std::cout << "GPU keyboard routing passed\n";
      return 0;
    }
#if JUCE_WINDOWS
    checkWindowsRendererOwnership();
#endif
    exercise();
    exercise();
    std::cout << "Native GPU surface, parameter editing, scope, resize, "
                 "hide/show, and repeated lifecycle passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
