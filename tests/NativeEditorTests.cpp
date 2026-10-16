#include "NativeEditorTests.h"
#include "NativeFileDrop.h"

#include "JucePath.h"
#include "Processor.h"
#include "ProcessorTestSupport.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"
#include "Vst3ControllerTestSupport.h"
#include "Vst3TestSupport.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
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

bool evaluate(juce::WebBrowserComponent &browser, const juce::String &script,
              const bool expected) {
  struct Result {
    bool complete{};
    bool passed{};
    juce::String error;
  };
  const auto result = std::make_shared<Result>();
  browser.evaluateJavascript(script, [result, expected](auto evaluation) {
    result->complete = true;
    if (const auto *value = evaluation.getResult())
      result->passed = value->isBool() && static_cast<bool>(*value) == expected;
    if (const auto *error = evaluation.getError())
      result->error = error->message;
  });
  if (!test::waitForMessage([&] { return result->complete; }) ||
      !result->passed) {
    std::cerr << "Native editor script failed: " << result->error << '\n';
    return false;
  }
  return true;
}

juce::WebBrowserComponent *readyBrowser(juce::AudioProcessorEditor &editor) {
  const auto *overlay = dynamic_cast<juce::Label *>(
      editor.findChildWithID("onda-loading-overlay"));
  if (!overlay ||
      !test::waitForMessage([&] { return !overlay->isVisible(); },
                            std::chrono::seconds(30))) {
    std::cerr << "Native editor did not acknowledge its initial state: "
              << (overlay ? overlay->getText() : "missing loading overlay")
              << '\n';
    return nullptr;
  }
  for (auto *child : editor.getChildren())
    if (auto *browser = dynamic_cast<juce::WebBrowserComponent *>(child))
      return browser;
  return nullptr;
}

} // namespace

bool exerciseNativeEditor() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "native-editor.onda")};
  if (!source.replaceWithText(R"(
outs { out1 }
buffers { clip: buffer<f32> }
params { enabled = false }
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}
sample { if enabled { out1 = 1.0 + clip[0] } else { out1 = 0.0 } }
)"))
    return false;

  for (const auto product : {Product::effect, Product::instrument}) {
    Processor processor(product);
    processor.setNonRealtime(true);
    {
      EditorWindow host(processor);
      auto *browser = readyBrowser(host.editor());
      if (!browser || !evaluate(*browser, R"(
        (() => {
          const picker = document.querySelector('#file-picker');
          const button = document.querySelector('#choose-onda-file');
          return !document.documentElement.hasAttribute('data-run-view-pending')
            && picker !== null && !picker.hidden
            && getComputedStyle(picker).visibility === 'visible'
            && button !== null && !button.disabled
            && button.getBoundingClientRect().width > 0;
        })()
      )", true))
        return false;
      const juce::StringArray files{"/unsupported.txt",
                                    source.getFullPathName()};
      auto *target =
          dynamic_cast<juce::FileDragAndDropTarget *>(&host.editor());
      if (!target || !target->isInterestedInFileDrag(files)) {
        std::cerr << "Editor does not accept an Onda file drag\n";
        return false;
      }
      if (target->isInterestedInFileDrag({"/unsupported.txt"}) ||
          target->isInterestedInFileDrag({"relative.onda"}) ||
          target->isInterestedInFileDrag({}))
        return false;
      if (!dropNativeFile(*browser, source))
        return false;
      if (!test::waitForMessage([&] {
            return processor.workerStatus().path ==
                   pathFromJuce(source.getFullPathName());
          })) {
        std::cerr << "Native browser did not forward the dropped path: "
                  << processor.workerStatus().path << ", expected "
                  << source.getFullPathName() << '\n';
        return false;
      }
      processor.prepareToPlay(48'000.0, 64);
      if (!processor.workerStatus().active ||
          processor.workerStatus().path !=
              pathFromJuce(source.getFullPathName())) {
        std::cerr << "Dropping an Onda file did not load it\n";
        return false;
      }
    }
    processor.loadFile(source);
    processor.prepareToPlay(48'000.0, 64);
    if (!processor.workerStatus().active)
      return false;
    juce::AudioBuffer<float> audio(
        std::max(pluginInputChannels, pluginOutputChannels), 64);
    juce::MidiBuffer midi;
    const auto renders = [&](float expected) {
      audio.clear();
      processor.processBlock(audio, midi);
      return std::all_of(audio.getReadPointer(0), audio.getReadPointer(0) + 64,
                         [&](float value) {
                           return test::withinTolerance(value - expected,
                                                        1.0e-6F);
                         });
    };
    for (const auto reopened : {false, true}) {
      EditorWindow host(processor);
      auto &editor = host.editor();
      if (!editor.getPeer() || !editor.getPeer()->getNativeHandle()) {
        std::cerr
            << "Native editor test host could not create a desktop window\n";
        return false;
      }
      auto *browser = readyBrowser(editor);
      if (!browser)
        return false;
      if (!renders(reopened ? 1.0F : 0.0F)) {
        std::cerr << "Native editor started with incorrect DSP state\n";
        return false;
      }
      if (!evaluate(*browser, R"(
        (() => {
          if (document.querySelector('#settings-popover').getClientRects().length)
            throw new Error('Unavailable device panel is visible');
          if (document.querySelector('#params-section [data-param-layout="knobs"]')
                .getAttribute('aria-pressed') !== 'true')
            throw new Error('Params must default to knobs');
          const buffers = document.querySelector('#buffers-section');
          if (getComputedStyle(buffers).display === 'none'
              || !document.querySelector('#buffers button'))
            throw new Error('Unbound buffer picker is missing');
          for (const button of document.querySelectorAll('#params-section [data-param-layout]')) {
            const box = button.getBoundingClientRect();
            const range = document.createRange();
            range.selectNodeContents(button);
            const text = range.getBoundingClientRect();
            if (Math.abs(text.left + text.right - box.left - box.right) > 1)
              throw new Error(button.textContent + ' layout label is not centered');
          }
          const input = document.querySelector('#params input[type=checkbox]');
          if (!input || input.disabled) throw new Error('Missing editable checkbox');
          if (getComputedStyle(input).visibility !== 'visible'
              || input.getBoundingClientRect().width <= 0)
            throw new Error('Checkbox is not visible');
          const checked = input.checked;
          input.click();
          return checked;
        })()
      )",
                                reopened))
        return false;
      const auto expected = reopened ? 0.0F : 1.0F;
      if (!test::waitForMessage([&] { return renders(expected); })) {
        std::cerr << "Native editor edit did not reach DSP\n";
        return false;
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
