#include "GpuView.h"
#include "GpuViewTestSupport.h"
#include <iostream>
#include <juce_gui_basics/juce_gui_basics.h>
#include <stdexcept>
#if JUCE_LINUX
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#undef KeyPress
#endif

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
#if JUCE_LINUX
void checkNativePixels(juce::Component &parent, onda::plugin::GpuView &view) {
  auto *display = XOpenDisplay(nullptr);
  check(display != nullptr, "Could not capture the native editor");
  const auto window = static_cast<::Window>(
      reinterpret_cast<uintptr_t>(parent.getPeer()->getNativeHandle()));
  const auto child = static_cast<::Window>(
      onda::plugin::GpuViewTestAccess::surfaceWindow(view));
  int count{}, ordering{};
  auto *shape =
      XShapeGetRectangles(display, child, ShapeInput, &count, &ordering);
  XFree(shape);
  check(count == 0, "GPU child intercepted JUCE's native input");
  parent.repaint();
  // Process the ordinary JUCE painter without any publication timer/tick.
  juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  XWindowAttributes attributes{};
  XGetWindowAttributes(display, window, &attributes);
  const auto bounds = parent.getPeer()->getAreaCoveredBy(view).toFloat() *
                      onda::plugin::GpuViewTestAccess::scale(view);
  const auto capture = bounds.getSmallestIntegerContainer().getIntersection(
      juce::Rectangle<int>{0, 0, attributes.width, attributes.height});
  auto *image =
      XGetImage(display, window, capture.getX(), capture.getY(),
                static_cast<unsigned>(capture.getWidth()),
                static_cast<unsigned>(capture.getHeight()), AllPlanes, ZPixmap);
  check(image != nullptr, "Native editor capture failed");
  int erased = 0;
  for (int y = 0; y < image->height; ++y)
    for (int x = 0; x < image->width; ++x)
      erased += (XGetPixel(image, x, y) & 0xffffff) == 0xff00ff;
  XDestroyImage(image);
  XCloseDisplay(display);
  check(erased == 0, "JUCE's parent painter erased GPU pixels");
}
#endif
void exerciseSurfaceOwnership() {
  Window first, second;
  for (auto *window : {&first, &second}) {
    window->setSize(720, 840);
    window->addToDesktop(0);
    window->setVisible(true);
  }
  int ready = 0;
  onda::plugin::GpuView view([](const auto &) {}, [&] { ++ready; });
  first.addAndMakeVisible(view);
  view.setBounds(first.getLocalBounds());
  pump(view);
  check(ready == 1, "Dedicated surface did not initialize");
  check(onda::plugin::GpuViewTestAccess::surfaceWindow(view) !=
            reinterpret_cast<uintptr_t>(first.getPeer()->getNativeHandle()),
        "GPU and JUCE still paint into the same native surface");
#if JUCE_LINUX || JUCE_WINDOWS
  // Host zoom/DPI changes may preserve every logical component bound.
  juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
  auto *peer = first.getPeer();
  const auto previousScale = peer->getCustomPlatformScaleFactor();
  const auto nextScale = peer->getPlatformScaleFactor() == 1.5 ? 2.0 : 1.5;
  const auto logicalBounds = view.getBounds();
  peer->setCustomPlatformScaleFactor(nextScale);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  check(
      view.getBounds() == logicalBounds &&
          juce::approximatelyEqual(onda::plugin::GpuViewTestAccess::scale(view),
                                   static_cast<float>(nextScale)) &&
          onda::plugin::GpuViewTestAccess::framebufferSize(view) ==
              juce::Point<int>{
                  juce::roundToInt(logicalBounds.getWidth() * nextScale),
                  juce::roundToInt(logicalBounds.getHeight() * nextScale)},
      "Idle DPI change waited for input, polling, or a logical resize");
  peer->setCustomPlatformScaleFactor(previousScale);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
#endif
#if JUCE_LINUX
  checkNativePixels(first, view);
#endif
  auto &render = onda::plugin::GpuViewTestAccess::render(view);
  static decltype(&onda_ui_render) nativeRender;
  nativeRender = render;
  static int renders;
  // A temporarily unavailable swapchain must leave the last image visible.
  render = [](OndaUi *) { return 0; };
  onda::plugin::GpuViewTestAccess::expose(view);
  view.tick();
#if JUCE_LINUX
  checkNativePixels(first, view);
#endif
  renders = 0;
  render = [](OndaUi *ui) {
    ++renders;
    return nativeRender(ui);
  };
  view.setSize(900, 700);
  check(renders > 0, "Resize waited for the publication timer to draw");
  render = nativeRender;
#if JUCE_LINUX
  checkNativePixels(first, view);
  // Moving the view must move its native child, preserving parent coordinates.
  view.setBounds(24, 30, 600, 600);
  checkNativePixels(first, view);
#endif
  view.setSize(0, 0);
  view.setBounds(first.getLocalBounds());
#if JUCE_LINUX
  checkNativePixels(first, view);
#endif
  second.addAndMakeVisible(view);
  pump(view);
  check(ready == 2, "Reparenting did not recreate the dedicated surface");
#if JUCE_LINUX || JUCE_WINDOWS
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  renders = 0;
  render = [](OndaUi *ui) {
    ++renders;
    return nativeRender(ui);
  };
  first.getPeer()->setCustomPlatformScaleFactor(1.75);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  check(renders == 0, "Reparented view still observed the old peer's scale");
  second.getPeer()->setCustomPlatformScaleFactor(1.75);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  check(renders > 0 && juce::approximatelyEqual(
                           onda::plugin::GpuViewTestAccess::scale(view), 1.75f),
        "Reparented view did not observe its new peer's scale");
  render = nativeRender;
#endif
  second.removeFromDesktop();
  pump(view);
  check(!onda::plugin::GpuViewTestAccess::initialized(view),
        "GPU retained a destroyed parent window");
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
              first.getPeer()->getCurrentRenderingEngine() == direct2D,
          "GPU surface changed the parent's rendering engine");
    first.repaint();
    pump(view);
    second.addAndMakeVisible(view);
    pump(view);
    check(ready == 2 &&
              first.getPeer()->getCurrentRenderingEngine() == direct2D &&
              second.getPeer()->getCurrentRenderingEngine() == direct2D,
          "Reparenting changed a parent's rendering engine");
  }
  check(second.getPeer()->getCurrentRenderingEngine() == direct2D,
        "Closing the GPU view changed the parent's renderer");
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
void exerciseScheduling() {
  using onda::plugin::GpuViewTestAccess;
  Window window;
  int saves = 0;
  onda::plugin::GpuView view(
      [&](const auto &command) {
        if (command["type"].toString() == "viewState")
          ++saves;
      },
      [] {});
  window.setSize(720, 840);
  window.addAndMakeVisible(view);
  view.setBounds(window.getLocalBounds());
  window.addToDesktop(0);
  window.setVisible(true);
  pump(view);
  view.message(juce::JSON::parse(
      R"({"type":"state","state":{"path":"schedule.onda","supportsScope":false,"supportsTransport":false,"supportsViewState":true,"paramLayout":"knobs","params":[{"name":"gain","type":"f64","value":0.5,"rangeMin":0,"rangeMax":1}]}})"));
  auto *messages = juce::MessageManager::getInstance();
  messages->runDispatchLoopUntil(50);
  static decltype(&onda_ui_render) nativeRender;
  static int renders;
  nativeRender = GpuViewTestAccess::render(view);
  const juce::ScopedValueSetter<decltype(nativeRender)> countRenders(
      GpuViewTestAccess::render(view), [](OndaUi *ui) {
        ++renders;
        return nativeRender(ui);
      });
  renders = 0;
  view.mouseDown(mouse(view, {102.0f, 179.0f}));
  for (int i = 1; i <= 20; ++i)
    view.mouseDrag(mouse(view, {102.0f, 179.0f - static_cast<float>(i)}));
  check(renders == 0, "Input rendered synchronously instead of coalescing");
  messages->runDispatchLoopUntil(30);
  // Native focus/exposure events may require another frame in this interval.
  if (renders != 1)
    std::cout << "Native input/exposure frames: " << renders << '\n';
  check(renders > 0, "Input burst did not draw without a polling tick");
  view.mouseUp(mouse(view, {102.0f, 159.0f}));
  view.mouseExit(mouse(view, {}));
  messages->runDispatchLoopUntil(150);
  renders = 0;
  messages->runDispatchLoopUntil(100);
  check(renders == 0 && !GpuViewTestAccess::wakePending(view),
        "An idle view kept drawing or waking");

  view.mouseMove(mouse(view, {100.0f, 35.0f}));
  messages->runDispatchLoopUntil(30);
  check(GpuViewTestAccess::wakePending(view),
        "Hover did not schedule a wakeup");
  renders = 0;
  messages->runDispatchLoopUntil(600);
  check(renders == 1 && !GpuViewTestAccess::wakePending(view),
        "Idle tooltip failed to draw once and return to sleep");
  view.mouseExit(mouse(view, {}));
  messages->runDispatchLoopUntil(30);

  // A temporary presentation failure must recover without a data polling timer.
  static int attempts;
  attempts = 0;
  GpuViewTestAccess::render(view) = [](OndaUi *ui) {
    if (++attempts == 1)
      return 0;
    return nativeRender(ui);
  };
  GpuViewTestAccess::expose(view);
  view.tick();
  check(attempts == 1 && GpuViewTestAccess::wakePending(view),
        "Unavailable presentation did not schedule a bounded retry");
  messages->runDispatchLoopUntil(100);
  check(attempts == 2 && !GpuViewTestAccess::wakePending(view),
        "Unavailable presentation did not recover and return to sleep");
  GpuViewTestAccess::render(view) = nativeRender;

  juce::Array<juce::var> params;
  for (int i = 0; i < 60; ++i)
    params.add(juce::JSON::parse("{\"name\":\"p" + juce::String(i) +
                                 "\",\"type\":\"f64\",\"value\":0}"));
  auto state =
      juce::JSON::parse(R"({"type":"state","state":{"path":"scroll.onda"}})");
  state["state"].getDynamicObject()->setProperty("params", params);
  view.message(state);
  messages->runDispatchLoopUntil(50);
  juce::MouseWheelDetails wheel{};
  wheel.deltaY = -0.1f;
  saves = 0;
  renders = 0;
  GpuViewTestAccess::render(view) = [](OndaUi *ui) {
    ++renders;
    return nativeRender(ui);
  };
  view.mouseWheelMove(mouse(view, {700.0f, 400.0f}), wheel);
  view.mouseExit(mouse(view, {}));
  messages->runDispatchLoopUntil(250);
  if (saves != 1 || renders != 1 || GpuViewTestAccess::wakePending(view))
    std::cerr << "Scroll scheduling: saves=" << saves << ", frames=" << renders
              << ", wake=" << GpuViewTestAccess::wakePending(view) << '\n';
  check(saves == 1 && renders == 1 && !GpuViewTestAccess::wakePending(view),
        "Scroll and its deferred save did not complete with one frame");
  view.mouseMove(mouse(view, {100.0f, 35.0f}));
  messages->runDispatchLoopUntil(30);
  window.setVisible(false);
  messages->runDispatchLoopUntil(30);
  check(!GpuViewTestAccess::wakePending(view),
        "Hidden view retained its wakeup timer");
}
void exerciseCompositionCancellation() {
  Window window;
  onda::plugin::GpuView view([](const auto &) {}, [] {});
  window.setSize(720, 840);
  window.addAndMakeVisible(view);
  view.setBounds(window.getLocalBounds());
  window.addToDesktop(0);
  window.setVisible(true);
  pump(view);
  for (
      const char *replacement :
      {R"({"path":"replacement.onda"})",
       R"({"params":[{"name":"cutoff","type":"f64","value":440,"rangeMin":20,"rangeMax":20000,"scale":"log"}]})"}) {
    view.message(juce::JSON::parse(
        R"({"type":"state","state":{"path":"composition.onda","running":true,"supportsScope":false,"paramLayout":"knobs",
          "params":[{"name":"frequency","type":"f64","value":440,"rangeMin":20,"rangeMax":20000,"scale":"log"}]}})"));
    const auto edit = [&] {
      view.mouseDown(mouse(view, {102.0f, 237.0f}));
      view.mouseUp(mouse(view, {102.0f, 237.0f}));
      check(view.isTextInputActive(), "Composition field did not activate");
    };
    edit();
    view.setTemporaryUnderlining({juce::Range<int>{0, 1}});
    view.message(juce::JSON::parse(
        R"({"type":"parameterValues","state":{"params":[{"name":"frequency","value":500}]}})"));
    check(!view.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)) &&
              view.isTextInputActive(),
          "A value update interrupted native composition");
    auto message = juce::JSON::parse(R"({"type":"state"})");
    message.getDynamicObject()->setProperty("state",
                                            juce::JSON::parse(replacement));
    view.message(message);
    check(!view.isTextInputActive(),
          "Replacement retained the composing draft");
    edit();
    check(view.keyPressed(juce::KeyPress('5', {}, '5')) &&
              view.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)) &&
              !view.isTextInputActive(),
          "Canceled composition blocked typing or Enter in the next field");
  }
}
void exercisePublicationFailure() {
  Window window;
  onda::plugin::GpuView view([](const auto &) {}, [] {});
  window.setSize(720, 840);
  window.addAndMakeVisible(view);
  view.setBounds(window.getLocalBounds());
  window.addToDesktop(0);
  window.setVisible(true);
  pump(view);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  // A failed publication must release a clean view, even when no frame is due.
  onda::plugin::GpuViewTestAccess::message(view) = [](OndaUi *, const char *) {
    return 0;
  };
  onda::plugin::GpuViewTestAccess::error(view) = [](const OndaUi *) {
    return "injected publication failure";
  };
  view.message(juce::JSON::parse(R"({"type":"state","state":{}})"));
  juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  check(view.error() == "injected publication failure" &&
            !onda::plugin::GpuViewTestAccess::initialized(view) &&
            !onda::plugin::GpuViewTestAccess::wakePending(view),
        "Failed publication retained a clean UI or pending wakeup");
}
void exerciseZeroSizeSuspension() {
  std::vector<juce::var> commands;
  Window window;
  onda::plugin::GpuView view(
      [&](const auto &command) { commands.push_back(command); }, [] {});
  window.setSize(720, 840);
  window.addAndMakeVisible(view);
  view.setBounds(window.getLocalBounds());
  window.addToDesktop(0);
  window.setVisible(true);
  pump(view);
  view.message(juce::JSON::parse(
      R"({"type":"state","state":{"path":"hidden.onda","running":true,"supportsScope":false,"supportsTransport":false,"midi":{"noteOn":true,"noteOff":true}}})"));
  view.mouseDown(mouse(view, {30.0f, 800.0f}));
  check(std::any_of(commands.begin(), commands.end(), [](const auto &command) {
          return command["type"].toString() == "midiNote" &&
                 static_cast<bool>(command["pressed"]);
        }),
        "Zero-size fixture did not acquire a MIDI note");
  commands.clear();
  view.setSize(0, 0);
  check(std::any_of(commands.begin(), commands.end(), [](const auto &command) {
          return command["type"].toString() == "midiNote" &&
                 !static_cast<bool>(command["pressed"]);
        }),
        "Zero-size surface retained a held MIDI note");
  commands.clear();
  view.setBounds(window.getLocalBounds());
  view.mouseUp(mouse(view, {30.0f, 800.0f}));
  check(std::none_of(commands.begin(), commands.end(), [](const auto &command) {
          return command["type"].toString() == "midiNote";
        }),
        "Restoring the surface released the same note twice");
}
void exerciseRenderFailure() {
  struct Scenario {
    const char *bank;
    const char *update;
    const char *terminal;
  };
  for (const auto &[bank, update, terminal] :
       {Scenario{"midi", "midiNote", "midiNote"},
        Scenario{"params", "setParam", "endParamGesture"},
        Scenario{"inputControls", "setInputControl", "endInputControlGesture"}}) {
    const bool piano = juce::String(bank) == "midi";
    std::vector<juce::var> commands;
    Window window;
    onda::plugin::GpuView view(
        [&](const auto &command) { commands.push_back(command); }, [] {});
    window.setSize(720, 840);
    window.addAndMakeVisible(view);
    view.setBounds(window.getLocalBounds());
    window.addToDesktop(0);
    window.setVisible(true);
    pump(view);
    auto message = juce::JSON::parse(
        R"({"type":"state","state":{"path":"failure.onda","running":true,"supportsTransport":false,"outputChannels":2,"midi":{"noteOn":true,"noteOff":true}}})");
    auto &state = *message["state"].getDynamicObject();
    state.setProperty("supportsScope", piano);
    if (!piano)
      state.setProperty(
          bank, juce::JSON::parse(
                    R"([{"name":"gain","type":"f64","value":0.5,"rangeMin":0,"rangeMax":1}])"));
    view.message(message);
    pump(view);
    if (piano) {
      check(view.scopeVisible() && view.meterVisible(),
            "Failure fixture has no visible visualization");
      view.mouseDown(mouse(view, {30.0f, 800.0f}));
    } else {
      view.mouseDown(mouse(view, {102.0f, 179.0f}));
      view.mouseDrag(mouse(view, {102.0f, 159.0f}));
    }
    check(std::any_of(commands.begin(), commands.end(), [&](const auto &c) {
            return c["type"].toString() == update &&
                   (piano ? static_cast<bool>(c["pressed"])
                          : !static_cast<bool>(c["commit"]));
          }),
          "Failure fixture did not acquire input ownership");
    view.tick();
    commands.clear();
    static int renders;
    renders = 0;
    onda::plugin::GpuViewTestAccess::render(view) = [](OndaUi *) {
      ++renders;
      return -1;
    };
    onda::plugin::GpuViewTestAccess::error(view) = [](const OndaUi *) {
      return "injected surface loss";
    };
    if (!piano)
      onda::plugin::GpuViewTestAccess::error(view) = [](const OndaUi *) {
        return "";
      };
    onda::plugin::GpuViewTestAccess::expose(view);
    view.tick();
    check(renders == 1, "Failure fixture did not request a render");
    check(std::any_of(commands.begin(), commands.end(), [&](const auto &c) {
            return c["type"].toString() == terminal &&
                   (piano ? !static_cast<bool>(c["pressed"])
                          : c["name"].toString() == "gain");
          }),
          "Fatal render error retained a MIDI note or automation gesture");
    check(!onda::plugin::GpuViewTestAccess::initialized(view) &&
              onda::plugin::GpuViewTestAccess::surfaceWindow(view) == 0 &&
              !view.scopeVisible() && !view.meterVisible(),
          "Fatal render error retained GPU resources or capture");
    for (int tick = 0; tick < 60; ++tick)
      view.tick();
    const auto expectedError = piano ? "injected surface loss"
                                    : "GPU UI rendering failed; reopen the editor";
    check(renders == 1 && view.error() == expectedError &&
              !view.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)),
          "Failed view retried rendering or accepted new input");
  }
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
      mouse(view, {102.0f, 237.0f}, juce::ModifierKeys::rightButtonModifier));
  view.mouseDrag(
      mouse(view, {154.0f, 237.0f}, juce::ModifierKeys::rightButtonModifier));
  view.mouseUp(
      mouse(view, {154.0f, 237.0f}, juce::ModifierKeys::rightButtonModifier));
  check(commands.empty(), "Right-click changed a parameter or started editing");
  view.keyPressed(juce::KeyPress('8', {}, '8'));
  view.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
  check(std::none_of(commands.begin(), commands.end(),
                     [](const auto &command) {
                       return command["type"].toString() == "setParam";
                     }),
        "Right-click entered a numeric draft");
  commands.clear();
  view.mouseMove(mouse(view, {102.0f, 179.0f}));
  check(view.getMouseCursor() == juce::MouseCursor::UpDownResizeCursor,
        "Knob hover did not update the native cursor");
  view.mouseMove(mouse(view, {102.0f, 237.0f}));
  check(view.getMouseCursor() == juce::MouseCursor::LeftRightResizeCursor,
        "Number hover did not expose horizontal dragging");
  view.mouseExit(mouse(view, {102.0f, 237.0f}));
  check(view.getMouseCursor() == juce::MouseCursor::NormalCursor,
        "Leaving the view retained a control cursor");
  view.mouseDown(mouse(view, {102.0f, 237.0f}));
  view.mouseUp(mouse(view, {102.0f, 237.0f}));
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
  view.mouseDown(mouse(view, {102.0f, 237.0f}));
  view.mouseUp(mouse(view, {102.0f, 237.0f}));
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
  view.mouseDown(mouse(view, {102.0f, 237.0f}));
  view.mouseUp(mouse(view, {102.0f, 237.0f}));
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
  view.mouseDown(mouse(view, {102.0f, 237.0f}));
  view.mouseDrag(mouse(view, {154.0f, 237.0f}));
  view.mouseUp(mouse(view, {154.0f, 237.0f}));
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
    presentation.getDynamicObject()->setProperty("scopePinned", true);
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
      R"({"type":"state","state":{"path":"collapsed.onda","viewState":{"scopePinned":true,"sections":{"scope":false}}}})"));
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
int runTests(int argc, const char *const *argv) {
  try {
    exerciseKeyboardRouting();
    if (argc == 2 && juce::String(argv[1]) == "--keyboard") {
      std::cout << "GPU keyboard routing passed\n";
      return 0;
    }
#if JUCE_WINDOWS
    checkWindowsRendererOwnership();
#endif
    exerciseCompositionCancellation();
    exercisePublicationFailure();
    exerciseZeroSizeSuspension();
    exerciseRenderFailure();
    exerciseSurfaceOwnership();
    exerciseScheduling();
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

class GpuUiTestApplication final : public juce::JUCEApplication {
public:
  GpuUiTestApplication(int argc, const char *const *argv)
      : argc_(argc), argv_(argv) {}
  const juce::String getApplicationName() override {
    return "Onda GPU UI tests";
  }
  const juce::String getApplicationVersion() override { return "1"; }
  bool moreThanOneInstanceAllowed() override { return true; }
  void initialise(const juce::String &) override {
    setApplicationReturnValue(runTests(argc_, argv_));
    quit();
  }
  void shutdown() override {}

private:
  int argc_;
  const char *const *argv_;
};

int main(int argc, const char **argv) {
  static const auto testArgc = argc;
  static const auto testArgv = argv;
  juce::JUCEApplicationBase::createInstance =
      []() -> juce::JUCEApplicationBase * {
    return new GpuUiTestApplication(testArgc, testArgv);
  };
  return juce::JUCEApplicationBase::main(argc, argv);
}
