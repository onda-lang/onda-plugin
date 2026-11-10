#include "NativeEditorTests.h"

#include "JucePath.h"
#include "GpuView.h"
#include "Processor.h"
#include "ProcessorTestSupport.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"
#include "Vst3ControllerTestSupport.h"
#include "Vst3TestSupport.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>

namespace {

// Own the host window and obey AudioProcessor's editor deletion protocol on
// every exit path. Both the native processor and exported VST3 use this host.
class EditorWindow {
public:
  explicit EditorWindow(juce::AudioProcessor &processor)
      : processor_(processor), editor_(processor.createEditorAndMakeActive()),
        window_("Onda editor test", juce::Colours::black, 0) {
    if (!editor_)
      throw std::runtime_error("The processor did not create an editor");
    window_.setContentNonOwned(editor_.get(), true);
    window_.setVisible(true);
  }

  ~EditorWindow() {
    window_.clearContentComponent();
    processor_.editorBeingDeleted(editor_.get());
  }

  juce::AudioProcessorEditor &editor() { return *editor_; }

private:
  juce::AudioProcessor &processor_;
  std::unique_ptr<juce::AudioProcessorEditor> editor_;
  juce::DocumentWindow window_;
};

onda::plugin::GpuView *readyGpu(juce::AudioProcessorEditor &editor) {
  const auto *overlay = dynamic_cast<juce::Label *>(
      editor.findChildWithID("onda-loading-overlay"));
  if (!overlay || !test::waitForMessage([&] { return !overlay->isVisible(); },
                                        std::chrono::seconds(30))) {
    std::cerr << "Native GPU editor did not initialize: "
              << (overlay ? overlay->getText() : "missing loading overlay") << '\n';
    return nullptr;
  }
  for (auto *child : editor.getChildren())
    if (auto *view = dynamic_cast<onda::plugin::GpuView *>(child))
      return view;
  return nullptr;
}

bool focusLevel(onda::plugin::GpuView &view, const char *expected) {
  view.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
  for (int step = 0; step < 32; ++step) {
    view.keyPressed(juce::KeyPress(juce::KeyPress::tabKey));
    if (view.isTextInputActive() &&
        view.getTextInRange({0, view.getTotalNumChars()}) == expected)
      return true;
  }
  std::cerr << "Native GPU editor did not expose the current parameter value\n";
  return false;
}

} // namespace

bool exerciseNativeEditor() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "native-editor.onda")};
  if (!source.replaceWithText(R"(
outs { out1 }
buffers { clip: buffer<f32>, spare: buffer<f32> }
params { level = 0.25 {0.0, 1.0} }
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}
sample { out1 = level + clip[0] + spare[0] }
)"))
    return false;

  for (const auto product : {Product::effect, Product::instrument}) {
    Processor processor(product);
    processor.setNonRealtime(true);
    // Test native edit delivery independently of sample-driven smoothing.
    if (!processor.setParamSmoothingSeconds(0.0))
      return false;
    {
      EditorWindow host(processor);
      if (!readyGpu(host.editor()))
        return false;
    }
    processor.loadFile(source);
    processor.prepareToPlay(48'000.0, 64);
    if (!processor.workerStatus().active) {
      std::cerr << "Native editor fixture did not compile: "
                << processor.workerStatus().message << '\n';
      return false;
    }
    juce::AudioBuffer<float> audio(
        std::max(pluginInputChannels, pluginOutputChannels), 64);
    juce::MidiBuffer midi;
    const auto renders = [&](float expected) {
      audio.clear();
      processor.processBlock(audio, midi);
      return std::all_of(audio.getReadPointer(0), audio.getReadPointer(0) + 64,
                         [&](float value) {
                           return test::withinTolerance(value - expected, 1.0e-6F);
                         });
    };
    for (const auto reopened : {false, true}) {
      EditorWindow host(processor);
      auto *view = readyGpu(host.editor());
      if (!view)
        return false;
      if (!test::waitForMessage([&] { return renders(reopened ? 0.75F : 0.25F); })) {
        std::cerr << "Native editor did not retain its parameter value\n";
        return false;
      }
      if (!focusLevel(*view, reopened ? "0.75" : "0.25"))
        return false;
      view->insertTextAtCaret(reopened ? "0.25" : "0.75");
      view->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
      const auto expected = reopened ? 0.25F : 0.75F;
      if (!test::waitForMessage([&] { return renders(expected); })) {
        std::cerr << "Native GPU parameter edit did not reach DSP\n";
        return false;
      }
      for (const auto value : {1.0F - expected, expected}) {
        processor.getParameters()[0]->setValue(value);
        test::dispatchMessages();
        if (!test::waitForMessage([&] {
              return focusLevel(*view, value > 0.5F ? "0.75" : "0.25");
            }))
          return false;
        view->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
      }
    }
    processor.releaseResources();
  }
  return true;
}

bool exerciseVst3EditorAttachment() {
  using namespace Steinberg;
  juce::VST3PluginFormat format;
  for (const auto *module : {ONDA_TEST_SYNTH_MODULE, ONDA_TEST_EFFECT_MODULE}) {
    const juce::File moduleFile{module};
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(
        descriptions, vst3BundleForModule(moduleFile).getFullPathName());
    if (descriptions.size() != 1) {
      std::cerr << "Expected one VST3 editor component in " << module << '\n';
      return false;
    }
    vst3_test::ObservedController controller;
    juce::String error;
    auto instance = format.createInstanceFromDescription(*descriptions[0],
                                                         48'000.0, 64, error);
    if (!instance || !instance->getVST3Client() ||
        !controller.connect(*instance->getVST3Client()->getIComponentPtr(),
                            moduleFile)) {
      std::cerr << "Could not connect the exported editor controller: " << error
                << '\n';
      return false;
    }
    for (int opening = 0; opening < 2; ++opening) {
      juce::Component window;
      auto view = controller.createView();
      ViewRect size{};
      if (!view || view->getSize(&size) != kResultOk || size.getWidth() <= 0 ||
          size.getHeight() <= 0)
        return false;
      window.setSize(size.getWidth(), size.getHeight());
      window.addToDesktop(0);
      window.setVisible(true);
      const auto *peer = window.getPeer();
      if (!peer || !peer->getNativeHandle()) {
        std::cerr
            << "VST3 editor test host could not create a desktop window\n";
        return false;
      }
      class Frame final : public vst3_test::HostInterface<IPlugFrame> {
      public:
        explicit Frame(juce::Component &owner) : owner_(owner) {}
        tresult PLUGIN_API resizeView(IPlugView *target,
                                      ViewRect *bounds) override {
          if (!target || !bounds)
            return kInvalidArgument;
          owner_.setSize(bounds->getWidth(), bounds->getHeight());
          return target->onSize(bounds);
        }

      private:
        juce::Component &owner_;
      } frame(window);
#if JUCE_WINDOWS
      const auto *type = kPlatformTypeHWND;
#elif JUCE_MAC
      const auto *type = kPlatformTypeNSView;
#else
      const auto *type = kPlatformTypeX11EmbedWindowID;
#endif
      if (view->isPlatformTypeSupported(type) != kResultTrue ||
          view->setFrame(&frame) != kResultOk ||
          view->attached(peer->getNativeHandle(), type) != kResultOk) {
        view->setFrame(nullptr);
        std::cerr << "VST3 editor rejected its native parent on opening "
                  << opening << '\n';
        return false;
      }
      tresult removed{kResultFalse};
      {
        const juce::ScopeGuard detach{[&] {
          removed = view->removed();
          view->setFrame(nullptr);
        }};
        test::dispatchMessages();
      }
      if (removed != kResultOk)
        return false;
    }
  }
  return true;
}
