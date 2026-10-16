#include "ParameterLifecycleTests.h"

#include "ControlInputTestSupport.h"
#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "StateFormat.h"
#include "TemporaryDirectory.h"
#include "TestAutomationQueue.h"
#include "Worker.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace onda::plugin;

constexpr auto midiEvents =
    "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
    "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n";

bool render(Processor &processor, const float expected) {
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  audio.clear();
  juce::MidiBuffer midi;
  if (!test::processRealtime(processor, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!std::is_eq(audio.getSample(0, frame) <=> expected)) {
      std::cerr << "parameter output: " << audio.getSample(0, frame)
                << ", expected " << expected << '\n';
      return false;
    }
  return true;
}

juce::String defaultsProgram(const juce::String &value,
                             const juce::String &range) {
  return "ins {\n" + test::mainAudioInputDeclaration() + "cv = " + value +
         " {" + range + "}\n}\n" + midiEvents +
         "outs { out }\nsample { out = cv }\n";
}

bool waitForReplacement(Processor &processor, std::uint64_t previous = 0) {
  const auto ready = test::waitUntil(
      [&] {
        const auto status = processor.workerStatus();
        return status.active && !status.compiling &&
               status.engineGeneration != previous;
      },
      test::yieldToWorker);
  if (!ready)
    std::cerr << "replacement failed: " << processor.workerStatus().message
              << '\n';
  return ready;
}

bool automationAfterUnload(const bool flush) {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "automation-after-unload.onda")};
  auto text = "ins {\n" + test::mainAudioInputDeclaration();
  if constexpr (pluginControlInputs > 0)
    text += "cv = 0.25 {0.0, 1.0}\n";
  text += "}\nparams { gain = 0.25 {0.0, 1.0} }\n" + juce::String{midiEvents} +
          "outs { out }\nsample { out = gain";
  if constexpr (pluginControlInputs > 0)
    text += " + cv";
  if (!source.replaceWithText(text + " }\n"))
    return false;

  for (const auto product : {Product::effect, Product::instrument}) {
    for (const auto acknowledged : {false, true}) {
      Processor processor(product);
      processor.prepareToPlay(48'000.0, 8);
      processor.loadFile(source);
      const auto defaultOutput = pluginControlInputs > 0 ? 0.5F : 0.25F;
      if (!waitForReplacement(processor) || !render(processor, defaultOutput))
        return false;
      if (acknowledged)
        test::service(processor);

      const auto checkValues = [&](const float expected) {
        if (!std::is_eq(processor.slotValue(0) <=> expected) ||
            (pluginControlInputs > 0 &&
             !std::is_eq(processor.getParameters()[static_cast<int>(slotCount)]
                             ->getValue() <=> expected))) {
          std::cerr << "automation after unload lost its endpoint (flush="
                    << flush << ", acknowledged=" << acknowledged << ")\n";
          return false;
        }
        return true;
      };
      const auto capture = [&](const TestAutomationQueue &queue,
                               const int frames) {
        if (!test::captureRealtime(
                processor,
                juce::VST3ClientExtensions::convertJuceParameterId("slot01"),
                queue.queue(), frames))
          return false;
        if constexpr (pluginControlInputs > 0)
          return test::captureRealtime(
              processor,
              juce::VST3ClientExtensions::convertJuceParameterId("input01"),
              queue.queue(), frames);
        return true;
      };
      if (!checkValues(acknowledged ? 0.25F : 0.5F))
        return false;
      if (!acknowledged) {
        // An endpoint retained by the old seed must not cross unload.
        const std::array stale{AutomationPoint{0, 0.625}};
        TestAutomationQueue queue{stale};
        if (!capture(queue, 8) || !render(processor, defaultOutput) ||
            !checkValues(0.5F))
          return false;
      }
      processor.unload();
      if (!test::auditRealtime([&] {
            processor.publishHostAutomationTargets();
            processor.clearHostAutomation();
          }) ||
          !checkValues(acknowledged ? 0.25F : 0.5F))
        return false;

      for (const auto endpoint : {0.8, 0.9}) {
        const std::array points{AutomationPoint{0, endpoint}};
        TestAutomationQueue queue{points};
        if (!capture(queue, flush ? 0 : 8))
          return false;
        if (flush) {
          // The VST3 wrapper skips DSP for a parameter-only process call.
          if (!test::auditRealtime([&] {
                processor.publishHostAutomationTargets();
                processor.clearHostAutomation();
              }))
            return false;
        } else if (!render(processor, 0.0F)) {
          return false;
        }
        if (!checkValues(static_cast<float>(endpoint)))
          return false;
        test::service(processor);
        if (!checkValues(static_cast<float>(endpoint)))
          return false;
      }

      // The last endpoint survives an empty callback, saving, and reloading.
      if (!render(processor, 0.0F) || !checkValues(0.9F))
        return false;
      juce::MemoryBlock saved;
      processor.getStateInformation(saved);
      Processor restored(product);
      restored.setStateInformation(saved.getData(),
                                   static_cast<int>(saved.getSize()));
      restored.loadFile(source, false);
      restored.prepareToPlay(48'000.0, 8);
      if (!render(restored, pluginControlInputs > 0 ? 1.8F : 0.9F))
        return false;
    }
  }
  return true;
}

juce::String unloadedStateProgram(const juce::String &value,
                                  const juce::String &range) {
  auto source = "ins {\n" + test::mainAudioInputDeclaration();
  if constexpr (pluginControlInputs > 0)
    source += "cv = " + value + " {" + range + "}\n";
  source += "}\nparams { gain: f64 = " + value + " {" + range + "} }\n" +
            juce::String{midiEvents} +
            "init { pinned = gain }\nouts { out }\n"
            "sample { out = f32(pinned)";
  if constexpr (pluginControlInputs > 0)
    source += " + cv";
  return source + " }\n";
}

bool unloadedRoundTrip(Processor &processor, const Product product,
                       const juce::File &source, const float expected) {
  juce::MemoryBlock saved;
  processor.getStateInformation(saved);
  for (const auto previouslyLoaded : {false, true}) {
    Processor restored(product);
    if (previouslyLoaded) {
      restored.loadFile(source);
      restored.prepareToPlay(48'000.0, 8);
    }
    restored.setStateInformation(saved.getData(),
                                 static_cast<int>(saved.getSize()));
    const auto status = restored.workerStatus();
    if (status.active || status.hasError || !status.path.empty())
      return false;
    // Resaving must preserve every normalized double and scalar domain, even
    // before an engine exists and after another explicit unload.
    for (const auto unloadAgain : {false, true}) {
      if (unloadAgain)
        restored.unload(false);
      juce::MemoryBlock resaved;
      restored.getStateInformation(resaved);
      if (resaved != saved) {
        std::cerr << "unloaded state lost precise values (previously loaded="
                  << previouslyLoaded << ", unload again=" << unloadAgain
                  << ")\n";
        return false;
      }
    }
    restored.loadFile(source, false);
    restored.prepareToPlay(48'000.0, 8);
    if (!render(restored, expected))
      return false;

    // Identical JUCE floats are still newer writes, and must invalidate both
    // restored endpoints and exact defaults before the next initialization.
    restored.getParameters()[0]->setValue(0.5F);
    if constexpr (pluginControlInputs > 0)
      restored.getParameters()[static_cast<int>(slotCount)]->setValue(0.5F);
    restored.requestReload();
    restored.prepareToPlay(48'000.0, 8);
    if (!render(restored, 0.0F))
      return false;
  }
  return true;
}

class ThreadListener final : public juce::AudioProcessorParameter::Listener {
public:
  explicit ThreadListener(juce::AudioProcessorParameter &parameter)
      : parameter_(parameter) {
    parameter_.addListener(this);
  }
  ~ThreadListener() override { parameter_.removeListener(this); }
  void parameterValueChanged(int, float) override {
    ++calls;
    onMessageThread =
        onMessageThread &&
        juce::MessageManager::getInstance()->isThisTheMessageThread();
  }
  void parameterGestureChanged(int, bool) override {}
  int calls{};
  bool onMessageThread{true};

private:
  juce::AudioProcessorParameter &parameter_;
};

// A host may cache values until a program/value refresh, independently of
// parameter presentation notifications and metadata changes.
class DefaultValueListener final : public juce::AudioProcessorListener {
public:
  explicit DefaultValueListener(Processor &processor) : processor_(processor) {
    processor_.addListener(this);
  }
  ~DefaultValueListener() override { processor_.removeListener(this); }
  void audioProcessorParameterChanged(juce::AudioProcessor *, int,
                                      float) override {}
  void audioProcessorChanged(juce::AudioProcessor *,
                             const ChangeDetails &details) override {
    if (!details.programChanged)
      return;
    ++refreshes;
    onMessageThread =
        onMessageThread &&
        juce::MessageManager::getInstance()->isThisTheMessageThread();
    parameter = processor_.slotValue(0);
    if constexpr (pluginControlInputs > 0)
      control = processor_.inputControlValue(0);
  }
  int refreshes{};
  bool onMessageThread{true};
  float parameter{0.5F}, control{0.5F};

private:
  Processor &processor_;
};

struct FlatQueue {
  double value{};
  mutable int reads{};
  static bool read(const void *context, int index,
                   AutomationPoint &point) noexcept {
    const auto &queue = *static_cast<const FlatQueue *>(context);
    ++queue.reads;
    point = {index, queue.value};
    return index >= 0 && index < 4096;
  }
  AutomationQueue queue() const { return {this, 4096, read}; }
};

bool parameterSnapshots() {
  ParameterValue value;
  constexpr auto first = 0.50000001;
  constexpr auto second = 0.50000002;
  const auto observed = value.target();
  const auto committed = value.publish(first, observed);
  if (!committed || !std::is_eq(value.preciseSnapshot().value <=> first) ||
      value.preciseSnapshot().stamp != committed->stamp)
    return false;
  // A failed publication must retain the current exact endpoint.
  if (value.publish(second, observed) ||
      !std::is_eq(value.preciseSnapshot().value <=> first))
    return false;

  std::atomic<bool> finished{};
  std::vector<ParameterValue::Target> publications{*committed};
  publications.reserve(100'001);
  std::vector<ParameterValue::Target> snapshots;
  snapshots.reserve(100'000);
  bool published = true;
  std::thread publisher([&] {
    for (int iteration = 0; iteration < 100'000; ++iteration) {
      if (const auto endpoint = value.publish(
              iteration % 2 == 0 ? second : first, value.target()))
        publications.push_back(*endpoint);
      else
        published = false;
    }
    finished.store(true, std::memory_order_release);
  });
  do {
    snapshots.push_back(value.preciseSnapshot());
  } while (!finished.load(std::memory_order_acquire) &&
           snapshots.size() < 100'000);
  publisher.join();
  if (!published)
    return false;
  for (const auto &snapshot : snapshots) {
    const auto endpoint = std::ranges::lower_bound(
        publications, snapshot.stamp, {}, &ParameterValue::Target::stamp);
    if (endpoint == publications.end() || endpoint->stamp != snapshot.stamp ||
        !std::is_eq(endpoint->value <=> snapshot.value))
      return false;
  }
  // Identical float writes still invalidate an older precise endpoint.
  const auto stamp = value.set(0.5F);
  const auto snapshot = value.preciseSnapshot();
  return snapshot.stamp == stamp && std::is_eq(snapshot.value <=> 0.5);
}

bool controlDefaultSnapshots() {
  ControlState controls;
  ControlValue first{0.5}, second{0.0625}, integer{1.0};
  first.scalarDefault = ControlValue::ScalarDefault{1.0F, {-1.0e30, 1.0e30}};
  second.scalarDefault = ControlValue::ScalarDefault{-3.5F, {-4.0, 4.0}};
  second.scalarDefault->domain.type = ScalarDomain::Type::f64;
  second.scalarDefault->domain.step = 0.5;
  second.scalarDefault->domain.stepCount = 16;
  second.scalarDefault->domain.curve = 1.5;
  second.scalarDefault->domain.hasCurve = true;
  ScalarDomain integerDomain{0.0, 1.0};
  integerDomain.type = ScalarDomain::Type::i64;
  integerDomain.defaultMayExceedRange = true;
  integer.scalarDefault = ScalarValue::ScalarDefault{
      std::int64_t{9007199254740993}, integerDomain};
  const std::array defaults{first, second, integer};
  controls.setDefault(0, first);
  std::atomic<bool> finished{};
  std::thread writer([&] {
    for (int iteration = 0; iteration < 100'000; ++iteration)
      controls.setDefault(0, defaults[static_cast<std::size_t>(iteration % 3)]);
    finished.store(true, std::memory_order_release);
  });
  bool coherent = true;
  const auto audited = test::auditRealtime([&] {
    do {
      const auto target = controls.target(0);
      if (const auto value = controls.defaultValue(0, target.stamp)) {
        const auto &expected =
            std::is_eq(target.value <=> first.normalized)    ? first
            : std::is_eq(target.value <=> second.normalized) ? second
                                                           : integer;
        coherent =
            coherent && std::is_eq(value->normalized <=> expected.normalized) &&
            std::is_eq(value->scalarDefault->plain <=>
                       expected.scalarDefault->plain) &&
            value->scalarDefault->domain == expected.scalarDefault->domain;
      }
    } while (!finished.load(std::memory_order_acquire));
  });
  writer.join();
  const auto last = controls.defaultValue(0, controls.target(0).stamp);
  if (!audited || !coherent || !last ||
      last->scalarDefault->plain != ScalarPlain{1.0}) {
    std::cerr << "Reset cache returned an incoherent scalar identity\n";
    return false;
  }
  // A direct host write repeating the normalized float still supersedes Reset.
  controls.set(0, 0.5F);
  return !controls.defaultValue(0, controls.target(0).stamp);
}

bool controlResetDuringAutomation(const Product product) {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "control-reset-during-automation.onda")};
  if (!source.replaceWithText(
          defaultsProgram("2.0", "-1000000000000000000000000000000.0, "
                                 "1000000000000000000000000000000.0")))
    return false;
  auto built = PreparedEngine::build(pathFromJuce(source.getFullPathName()),
                                     product, 48'000.0, 3);
  if (!built.engine)
    return false;
  ParameterState slots;
  ControlState controls;
  HostAutomation<controlCount> automation{controls};
  const auto defaultValue = built.engine->controlMappings()[0].controlDefault();
  controls.setDefault(0, defaultValue);
  struct ResetQueue {
    ControlState &controls;
    const ControlValue &defaultValue;
    mutable bool reset{};
    static bool read(const void *context, const int index,
                     AutomationPoint &point) noexcept {
      const auto &queue = *static_cast<const ResetQueue *>(context);
      if (index < 0 || index > 1)
        return false;
      // Emulate Reset's write after endpoint publication, when the cursor
      // first reads the curve. The edit itself remains lock-free.
      if (index == 0 && queue.reset) {
        queue.controls.setDefault(0, queue.defaultValue);
        queue.reset = false;
      }
      point = index == 0 ? AutomationPoint{0, 0.75} : AutomationPoint{7, 1.0};
      return true;
    }
  } queue{controls, defaultValue};
  juce::AudioBuffer<float> audio(pluginOutputChannels, 8);
  const auto renderDefault = [&] {
    auto succeeded = false;
    if (!test::auditRealtime([&] {
          built.engine->beginHostCallback(8, nullptr, &controls,
                                          &automation.curves);
          succeeded = built.engine->process(
              nullptr, audio.getArrayOfWritePointers(), 8, {}, slots);
        }) ||
        !succeeded)
      return false;
    for (int frame = 0; frame < 8; ++frame)
      if (!std::is_eq(audio.getSample(0, frame) <=> 2.0F))
        return false;
    return true;
  };
  if (!renderDefault() || !test::auditRealtime([&] {
        automation.capture(0, {&queue, 2, ResetQueue::read}, 8, 0);
        automation.publish(0, false);
      }))
    return false;
  queue.reset = true;
  if (!renderDefault())
    return false;
  automation.curves.clear();
  return renderDefault();
}

bool resetDuringBuild() {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "control-reset-during-build.onda")};
  constexpr auto range = "-1000000000.0, 1000000000.0";
  if (!source.replaceWithText(defaultsProgram("1.0", range)))
    return false;
  ParameterState slots;
  ControlState controls;
  SpscSlot<PreparedEngine *> replacements, retirements;
  std::atomic<bool> deactivate{}, gate{}, entered{};
  std::mutex mutex;
  std::condition_variable condition;
  bool released{};
  Worker worker{
      Product::effect,
      replacements,
      retirements,
      deactivate,
      [&](const std::filesystem::path &path, Product product, double rate,
          int block, std::span<const BufferFileBinding> buffers,
          const std::optional<ParameterValues> &parameters) {
        auto built = PreparedEngine::build(path, product, rate, block, buffers,
                                           parameters);
        if (gate.load(std::memory_order_acquire)) {
          std::unique_lock lock(mutex);
          entered.store(true, std::memory_order_release);
          condition.wait_for(lock, test::waitTimeout, [&] { return released; });
        }
        return built;
      },
      nullptr,
      [&] {
        HostParameterValues values;
        for (std::size_t index = 0; index < controlCount; ++index) {
          const auto target = controls.snapshot(index);
          values.controls[index] = target.value;
          values.controlStamps[index] = target.stamp;
        }
        return values;
      }};
  worker.load(pathFromJuce(source.getFullPathName()), true);
  worker.configure(48'000.0, 8);
  static_cast<void>(worker.waitForPreparation());
  std::unique_ptr<PreparedEngine> first{replacements.tryPop()};
  if (!first)
    return false;
  worker.setActiveGeneration(first->buildGeneration());
  auto *seed = first->parameterSeed();
  if (!seed)
    return false;
  seed->controlStamps.before[0] = controls.snapshot(0).stamp;
  seed->started.store(true, std::memory_order_release);
  const auto claimed = worker.takeSeedValues();
  if (!claimed)
    return false;
  static_cast<void>(
      controls.seed(0, static_cast<float>(seed->controls[0].normalized),
                    seed->controlStamps.before[0], seed->controlStamps.after[0]));
  worker.finishSeeding(claimed);

  // A host write between restoration's value commit and the worker handoff
  // must invalidate the saved scalar, even if it repeats the same float.
  HostParameterValues saved;
  saved.controls[0] = first->controlMappings()[0].controlDefault();
  saved.controlStamps[0] = controls[0].set(0.5F);
  controls.set(0, 0.5F);
  worker.restore(pathFromJuce(source.getFullPathName()), {}, {},
                 Worker::ExistingEnginePolicy::deactivateImmediately, saved);
  const auto restored = worker.persistedStateSnapshot();
  if (restored.values.controls[0].scalarDefault ||
      !std::is_eq(restored.values.controls[0].normalized <=> 0.5)) {
    std::cerr << "restoration replaced a newer host control write\n";
    return false;
  }
  static_cast<void>(worker.waitForPreparation());
  std::unique_ptr<PreparedEngine> restoredEngine{replacements.tryPop()};
  if (!restoredEngine)
    return false;
  worker.setActiveGeneration(restoredEngine->buildGeneration());

  gate.store(true, std::memory_order_release);
  if (!source.replaceWithText(defaultsProgram("2.0", range)))
    return false;
  worker.requestRebuild();
  const auto ready =
      test::waitUntil([&] { return entered.load(std::memory_order_acquire); },
                      test::yieldToWorker);
  if (ready) {
    static_cast<void>(worker.resetParameters(slots, controls));
  }
  {
    std::lock_guard lock(mutex);
    released = true;
  }
  condition.notify_one();
  if (!ready)
    return false;
  static_cast<void>(worker.waitForPreparation());
  std::unique_ptr<PreparedEngine> replacement{replacements.tryPop()};
  if (!replacement)
    return false;
  juce::AudioBuffer<float> audio(pluginOutputChannels, 8);
  const auto *outputs = audio.getArrayOfWritePointers();
  auto rendered = false;
  if (!test::auditRealtime([&] {
        replacement->beginHostCallback(8, nullptr, &controls);
        rendered = replacement->process(nullptr, outputs, 8, {}, slots);
      }) ||
      !rendered)
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!std::is_eq(audio.getSample(0, frame) <=> 1.0F)) {
      std::cerr
          << "Reset during compilation adopted a different scalar default\n";
      return false;
    }
  return true;
}

bool resetDuringReplacement(const Product product) {
  const juce::File source{pathToJuce(testTemporaryRoot() /
                                     "control-reset-during-replacement.onda")};
  constexpr auto range = "-1000000000000000000000000000000.0, "
                         "1000000000000000000000000000000.0";
  if (!source.replaceWithText(defaultsProgram("1.0", range)))
    return false;
  Processor processor(product);
  processor.prepareToPlay(48'000.0, 8);
  processor.loadFile(source);
  processor.prepareToPlay(48'000.0, 8);
  if (!render(processor, 1.0F))
    return false;

  struct ResetQueue {
    Processor &processor;
    const juce::File &source;
    juce::String replacement;
    mutable bool triggered{}, succeeded{};
    static bool read(const void *context, const int index,
                     AutomationPoint &point) noexcept {
      const auto &queue = *static_cast<const ResetQueue *>(context);
      if (index < 0 || index > 1)
        return false;
      // Capture reads the last point. The cursor reads the first only after
      // this callback has acquired its engine. Force publication and Reset
      // there to reproduce the worker/UI/audio interleaving deterministically.
      if (index == 0 && !queue.triggered) {
        queue.triggered = true;
        const auto generation = queue.processor.workerStatus().engineGeneration;
        if (!queue.source.replaceWithText(queue.replacement))
          return false;
        queue.processor.requestReload();
        if (!waitForReplacement(queue.processor, generation))
          return false;
        queue.processor.resetParametersToDefaults();
        queue.succeeded = true;
      }
      point = index == 0 ? AutomationPoint{0, 0.75} : AutomationPoint{7, 1.0};
      return true;
    }
  } queue{processor, source, defaultsProgram("2.0", range)};
  const auto id = juce::VST3ClientExtensions::convertJuceParameterId("input01");
  processor.captureHostAutomation(id, {&queue, 2, ResetQueue::read}, 8);
  processor.publishHostAutomationTargets();
  // The injected publication deliberately compiles and notifies listeners;
  // it is a scheduling probe, so audit the subsequent callback separately.
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  audio.clear();
  juce::MidiBuffer midi;
  processor.processBlock(audio, midi);
  processor.clearHostAutomation();
  if (!queue.succeeded)
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!std::is_eq(audio.getSample(0, frame) <=> 2.0F)) {
      std::cerr << "Reset used the active engine's older scalar default: "
                << audio.getSample(0, frame) << '\n';
      return false;
    }
  if (!render(processor, 2.0F))
    return false;
  juce::MemoryBlock state;
  processor.getStateInformation(state);
  Processor restored(product);
  restored.setStateInformation(state.getData(),
                               static_cast<int>(state.getSize()));
  restored.prepareToPlay(48'000.0, 8);
  return render(restored, 2.0F);
}
} // namespace

bool exerciseUnloadedEndpointPrecision() {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "unloaded-endpoint-precision.onda")};
  if (!source.replaceWithText(
          unloadedStateProgram("0.0", "-1000000000.0, 1000000000.0")))
    return false;
  for (const auto product : {Product::effect, Product::instrument}) {
    Processor processor(product);
    const std::array points{AutomationPoint{0, 0.50000001}};
    TestAutomationQueue queue{points};
    for (const auto id : {"slot01", "input01"}) {
      if (juce::String{id} == "input01" && pluginControlInputs == 0)
        continue;
      if (!test::captureRealtime(
              processor, juce::VST3ClientExtensions::convertJuceParameterId(id),
              queue.queue(), 0))
        return false;
    }
    if (!test::auditRealtime([&] {
          processor.publishHostAutomationTargets();
          processor.clearHostAutomation();
        }) ||
        !unloadedRoundTrip(processor, product, source,
                           pluginControlInputs > 0 ? 40.0F : 20.0F))
      return false;
  }
  return true;
}

bool exerciseUnloadedDefaultPrecision() {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "unloaded-default-precision.onda")};
  if (!source.replaceWithText(unloadedStateProgram(
          "1.0", "-1000000000000000000000000000000.0, "
                 "1000000000000000000000000000000.0")))
    return false;
  for (const auto product : {Product::effect, Product::instrument}) {
    Processor processor(product);
    processor.loadFile(source);
    processor.prepareToPlay(48'000.0, 8);
    processor.resetParametersToDefaults();
    processor.unload();
    if (!unloadedRoundTrip(processor, product, source,
                           pluginControlInputs > 0 ? 2.0F : 1.0F))
      return false;
  }
  return true;
}

bool exerciseParameterEndpointPrecision() {
  if (!parameterSnapshots()) {
    std::cerr << "concurrent parameter snapshots lost exact endpoints\n";
    return false;
  }
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "parameter-initializer-precision.onda")};
  if (!source.replaceWithText(
          juce::String(midiEvents) +
          "params { p: f64 = 0.5 {0.0, 1.0} }\nouts { out }\n"
          "init { pinned = p }\n"
          "sample { out = f32((p - pinned) * 100000000.0) }\n"))
    return false;
  for (const auto product : {Product::effect, Product::instrument}) {
    Processor processor(product);
    processor.loadFile(source);
    processor.prepareToPlay(48'000.0, 8);
    const std::array points{AutomationPoint{0, 0.50000001}};
    TestAutomationQueue queue{points};
    const auto id =
        juce::VST3ClientExtensions::convertJuceParameterId("slot01");
    if (!test::captureRealtime(processor, id, queue.queue(), 8) ||
        !render(processor, 1.0F))
      return false;
    processor.requestReload();
    processor.prepareToPlay(48'000.0, 8);
    if (!render(processor, 0.0F)) {
      std::cerr << "reloaded initializer received a rounded endpoint\n";
      return false;
    }
    // A newer float write must also reach the next initializer.
    processor.getParameters()[0]->setValue(0.5F);
    processor.requestReload();
    processor.prepareToPlay(48'000.0, 8);
    if (!render(processor, 0.0F))
      return false;
  }
  return true;
}

bool exerciseControlEndpointPrecision() {
  if constexpr (pluginControlInputs == 0)
    return true;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "control-endpoint-precision.onda")};
  if (!source.replaceWithText(
          defaultsProgram("0.0", "-1000000000.0, 1000000000.0")))
    return false;
  for (const auto product : {Product::effect, Product::instrument}) {
    Processor processor(product);
    processor.loadFile(source);
    processor.prepareToPlay(48'000.0, 8);
    const std::array points{AutomationPoint{0, 0.50000001}};
    TestAutomationQueue queue{points};
    const auto id =
        juce::VST3ClientExtensions::convertJuceParameterId("input01");
    if (!test::captureRealtime(processor, id, queue.queue(), 8) ||
        !render(processor, 20.0F) || !render(processor, 20.0F))
      return false;
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    Processor restored(product);
    restored.setStateInformation(saved.getData(),
                                 static_cast<int>(saved.getSize()));
    restored.prepareToPlay(48'000.0, 8);
    if (!render(restored, 20.0F)) {
      std::cerr << "restored control lost its exact endpoint\n";
      return false;
    }
    processor.requestReload();
    processor.prepareToPlay(48'000.0, 8);
    if (!render(processor, 20.0F)) {
      std::cerr << "reloaded control lost its exact endpoint\n";
      return false;
    }
    processor.getParameters()[static_cast<int>(slotCount)]->setValue(0.5F);
    if (!render(processor, 0.0F) || !controlResetDuringAutomation(product))
      return false;
  }
  return true;
}

bool exerciseControlDefaultPrecision() {
  if constexpr (pluginControlInputs == 0)
    return true;
  if (!controlDefaultSnapshots())
    return false;
  // Valid f32 endpoint defaults can round outside the metadata's double range.
  ParameterMapping fractional;
  fractional.defaultPlain = fractional.rangeMin = fractional.rangeMax = 0.1;
  fractional.defaultNormalized = 0.0;
  juce::MemoryBlock fractionalState;
  juce::MemoryOutputStream fractionalOutput(fractionalState, false);
  writeScalarValue(fractionalOutput, fractional.controlDefault());
  juce::MemoryInputStream fractionalInput(fractionalState, false);
  const auto fractionalRestored = readScalarValue(fractionalInput);
  if (!fractionalRestored ||
      !std::is_eq(fractionalRestored->plain(0.1, 0.1) <=> 0.1F))
    return false;
  for (const auto product : {Product::effect, Product::instrument}) {
    if (!resetDuringReplacement(product))
      return false;
    for (const auto range :
         {"-1000000000.0, 1000000000.0", "-1000000000000000000000000000000.0, "
                                         "1000000000000000000000000000000.0"}) {
      const juce::File source{
          pathToJuce(testTemporaryRoot() / "precise-default.onda")};
      if (!source.replaceWithText(defaultsProgram("1.0", range)))
        return false;
      Processor processor(product);
      processor.prepareToPlay(48'000.0, 8);
      processor.loadFile(source);
      if (!waitForReplacement(processor) || !render(processor, 1.0F))
        return false;
      juce::MemoryBlock pending, acknowledged;
      processor.getStateInformation(pending);
      test::service(processor);
      if (!render(processor, 1.0F))
        return false;
      processor.getStateInformation(acknowledged);
      for (const auto *state : {&pending, &acknowledged}) {
        Processor restored(product);
        restored.setStateInformation(state->getData(),
                                     static_cast<int>(state->getSize()));
        restored.prepareToPlay(48'000.0, 8);
        if (!render(restored, 1.0F))
          return false;
      }
      const auto generation = processor.workerStatus().engineGeneration;
      if (!source.replaceWithText(defaultsProgram("2.0", range)))
        return false;
      processor.requestReload();
      if (!waitForReplacement(processor, generation) ||
          !render(processor, 1.0F))
        return false;
      test::service(processor);
      if (!render(processor, 1.0F))
        return false;
      processor.resetParametersToDefaults();
      if (!render(processor, 2.0F))
        return false;
      processor.getStateInformation(acknowledged);
      Processor restored(product);
      restored.setStateInformation(acknowledged.getData(),
                                   static_cast<int>(acknowledged.getSize()));
      restored.prepareToPlay(48'000.0, 8);
      if (!render(restored, 2.0F))
        return false;
      // An identical float write is still a host edit and supersedes a default.
      processor.getParameters()[static_cast<int>(slotCount)]->setValue(0.5F);
      if (!render(processor, 0.0F))
        return false;
      processor.resetParametersToDefaults();
      if (!render(processor, 2.0F))
        return false;
      const auto id =
          juce::VST3ClientExtensions::convertJuceParameterId("input01");
      const std::array points{AutomationPoint{0, 0.5}};
      TestAutomationQueue queue{points};
      if (!test::captureRealtime(processor, id, queue.queue(), 8) ||
          !render(processor, 0.0F))
        return false;
    }
  }
  return resetDuringBuild();
}

bool exerciseSeedNotificationThreads() {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "seed-notification-thread.onda")};
  if (!source.replaceWithText(juce::String(midiEvents) +
                              "params { level = 0.25 {0.0, 1.0} }\n"
                              "outs { out }\nsample { out = level }\n"))
    return false;
  for (const auto product : {Product::effect, Product::instrument}) {
    for (const auto prepare : {false, true}) {
      Processor processor(product);
      processor.prepareToPlay(48'000.0, 8);
      ThreadListener listener{*processor.getParameters()[0]};
      DefaultValueListener defaults(processor);
      processor.loadFile(source);
      std::thread processing([&] {
        if (prepare) {
          processor.prepareToPlay(48'000.0, 8);
        } else {
          processor.setNonRealtime(true);
          juce::AudioBuffer<float> audio(
              std::max(pluginInputChannels, pluginOutputChannels), 8);
          juce::MidiBuffer midi;
          processor.processBlock(audio, midi);
        }
      });
      processing.join();
      if (listener.calls != 0 || defaults.refreshes != 0) {
        std::cerr << "slot seed listeners ran during processing/preparation\n";
        return false;
      }
      test::service(processor);
      if (listener.calls == 0 || !listener.onMessageThread ||
          defaults.refreshes != 1 || !defaults.onMessageThread ||
          !std::is_eq(defaults.parameter <=> 0.25F)) {
        std::cerr
            << "slot seed notification was not delivered on the message thread"
            << " (prepare=" << prepare << ", calls=" << listener.calls
            << ", message thread=" << listener.onMessageThread
            << ", status=" << processor.workerStatus().message << ")\n";
        return false;
      }
    }
  }
  return true;
}

bool exerciseParameterDefaultNotifications() {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "parameter-default-notifications.onda")};
  const auto program = [&](const bool control) {
    auto text = juce::String{};
    if (control)
      text = "ins {\n" + test::mainAudioInputDeclaration() +
             "cv = 0.75 {0.0, 1.0}\n}\n";
    text += juce::String{midiEvents} +
            "params { level = 0.25 {0.0, 1.0} }\nouts { out }\n"
            "sample { out = level";
    return text + (control ? " + cv" : "") + " }\n";
  };
  for (const auto product : {Product::effect, Product::instrument}) {
    if (!source.replaceWithText(program(false)))
      return false;
    Processor processor(product);
    processor.prepareToPlay(48'000.0, 8);
    DefaultValueListener defaults(processor);
    processor.loadFile(source);
    if (!waitForReplacement(processor) || !render(processor, 0.25F) ||
        defaults.refreshes != 0)
      return false;
    test::service(processor); // Commit the selected patch's defaults.
    test::service(processor); // Refresh the host after controller delivery.
    if (defaults.refreshes != 1 || !defaults.onMessageThread ||
        !std::is_eq(defaults.parameter <=> 0.25F)) {
      std::cerr << "selected patch did not refresh host defaults\n";
      return false;
    }
    if constexpr (pluginControlInputs > 0) {
      const auto previous = processor.workerStatus().engineGeneration;
      if (!source.replaceWithText(program(true)))
        return false;
      processor.requestReload();
      if (!waitForReplacement(processor, previous) ||
          !render(processor, 1.0F) || defaults.refreshes != 1)
        return false;
      test::service(processor);
      if (defaults.refreshes != 2 || !defaults.onMessageThread ||
          !std::is_eq(defaults.control <=> 0.75F)) {
        std::cerr << "new control did not refresh host defaults\n";
        return false;
      }
    }
    const auto refreshes = defaults.refreshes;
    const std::array points{AutomationPoint{0, 0.625}};
    TestAutomationQueue queue{points};
    const auto id =
        juce::VST3ClientExtensions::convertJuceParameterId("slot01");
    if (!test::captureRealtime(processor, id, queue.queue(), 8) ||
        !render(processor, pluginControlInputs > 0 ? 1.375F : 0.625F))
      return false;
    processor.clearHostAutomation();
    test::service(processor);
    if (defaults.refreshes != refreshes) {
      std::cerr << "incoming automation requested a host value refresh\n";
      return false;
    }
    const auto previous = processor.workerStatus().engineGeneration;
    processor.requestReload();
    if (!waitForReplacement(processor, previous) ||
        !render(processor, pluginControlInputs > 0 ? 1.375F : 0.625F))
      return false;
    test::service(processor);
    test::service(processor);
    if (defaults.refreshes != refreshes) {
      std::cerr << "unchanged defaults requested a host value refresh\n";
      return false;
    }
  }
  return true;
}

bool exerciseAutomationAfterUnload() {
  return automationAfterUnload(false);
}

bool exerciseAutomationFlushAfterUnload() {
  return automationAfterUnload(true);
}

bool exerciseAutomationFlushAfterFault() {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "automation-flush-after-fault.onda")};
  auto text = "ins {\n" + test::mainAudioInputDeclaration();
  if constexpr (pluginControlInputs > 0)
    text += "cv = 0.25 {0.0, 1.0}\n";
  text += "}\nparams { gain = 0.25 {0.0, 1.0} }\n" + juce::String{midiEvents} +
          "outs { out }\ninit { divisor = i32(0) }\n"
          "def quotient(value: i32, by: i32) { return value / by }\n"
          "sample { out = f32(quotient(i32(1), divisor)) }\n";
  if (!source.replaceWithText(text))
    return false;

  const auto checkValues = [](const Processor &processor,
                              const double endpoint) {
    const auto values = processor.hostParameterValues();
    if (!std::is_eq(processor.slotValue(0) <=> static_cast<float>(endpoint)) ||
        !std::is_eq(values.parameters[0].normalized <=> endpoint) ||
        values.parameters[0].scalarDefault ||
        (pluginControlInputs > 0 &&
         (!std::is_eq(processor.inputControlValue(0) <=>
                      static_cast<float>(endpoint)) ||
          !std::is_eq(values.controls[0].normalized <=> endpoint) ||
          values.controls[0].scalarDefault))) {
      std::cerr << "automation after a runtime fault lost its endpoint\n";
      return false;
    }
    return true;
  };

  for (const auto product : {Product::effect, Product::instrument}) {
    for (const auto acknowledged : {false, true}) {
      Processor processor(product);
      processor.prepareToPlay(48'000.0, 8);
      processor.loadFile(source);
      if (!waitForReplacement(processor))
        return false;
      if (acknowledged)
        processor.prepareToPlay(48'000.0, 8);

      // The first render faults before the message thread can acknowledge an
      // asynchronously adopted seed. No later audio callback may retire it.
      if (!render(processor, 0.0F) ||
          processor.workerStatus().message.find("runtime safety check") ==
              std::string::npos ||
          !std::is_eq(processor.slotValue(0) <=> (acknowledged ? 0.25F : 0.5F)))
        return false;

      for (const auto endpoint : {0.8, 0.9}) {
        const std::array points{AutomationPoint{0, endpoint}};
        TestAutomationQueue queue{points};
        for (const auto id : {"slot01", "input01"}) {
          if (juce::String{id} == "input01" && pluginControlInputs == 0)
            continue;
          if (!test::captureRealtime(
                  processor,
                  juce::VST3ClientExtensions::convertJuceParameterId(id),
                  queue.queue(), 0))
            return false;
        }
        if (!test::auditRealtime([&] {
              processor.publishHostAutomationTargets();
              processor.clearHostAutomation();
            }) ||
            !checkValues(processor, endpoint))
          return false;
        test::service(processor);
        if (!checkValues(processor, endpoint))
          return false;
      }

      juce::MemoryBlock saved;
      processor.getStateInformation(saved);
      Processor restored(product);
      restored.setStateInformation(saved.getData(),
                                   static_cast<int>(saved.getSize()));
      restored.prepareToPlay(48'000.0, 8);
      if (!checkValues(restored, 0.9))
        return false;
    }
  }
  return true;
}

bool exerciseAutomationEchoWork() {
  if constexpr (pluginControlInputs == 0)
    return true;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "automation-echo-work.onda")};
  auto text = defaultsProgram("0.75", "0.0, 1.0");
  text = text.replace("out = cv", "out = cv + level");
  text += "params { level = 0.75 {0.0, 1.0} }\n";
  if (!source.replaceWithText(text))
    return false;
  Processor processor(Product::effect);
  processor.prepareToPlay(48'000.0, 4096);
  if (!processor.setPreferredBlockSize(8))
    return false;
  processor.loadFile(source);
  processor.prepareToPlay(48'000.0, 4096);
  if (!render(processor, 1.5F))
    return false;
  processor.resetParametersToDefaults();
  if (!render(processor, 1.5F))
    return false;
  juce::StringArray ids{"input01", "slot01"};
  if constexpr (slotCount > 1)
    ids.add("slot" + juce::String(static_cast<int>(slotCount)).paddedLeft('0', 2));
  for (const auto &id : ids) {
    const FlatQueue queue{id == "input01" || id == "slot01" ? 0.75 : 0.5};
    if (!test::captureRealtime(
            processor, juce::VST3ClientExtensions::convertJuceParameterId(id),
            queue.queue(), 4096) ||
        queue.reads != 1) {
      std::cerr << id << " stationary queue read " << queue.reads
                << " points\n";
      return false;
    }
    processor.publishHostAutomationTargets();
    processor.clearHostAutomation();
  }

  // Matching host feedback still preserves a continuous slot's editor ramp.
  processor.setSlotValue(0, 1.0F);
  const std::array feedback{AutomationPoint{0, 1.0}};
  TestAutomationQueue echo{feedback};
  const auto slot =
      juce::VST3ClientExtensions::convertJuceParameterId("slot01");
  for (const auto elapsed : {8, 16}) {
    const auto expected =
        0.75F + static_cast<float>(std::lerp(0.75, 1.0, elapsed / 1440.0));
    if (!test::captureRealtime(processor, slot, echo.queue(), 8) ||
        !render(processor, expected))
      return false;
  }
  juce::AudioBuffer<float> remaining(
      std::max(pluginInputChannels, pluginOutputChannels), 1440);
  remaining.clear();
  juce::MidiBuffer midi;
  if (!test::processRealtime(processor, remaining, midi))
    return false;
  const FlatQueue settled{1.0};
  if (!test::captureRealtime(processor, slot, settled.queue(), 4096) ||
      settled.reads != 1)
    return false;
  processor.clearHostAutomation();
  return true;
}
