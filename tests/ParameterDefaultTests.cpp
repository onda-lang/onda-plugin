#include "ParameterDefaultTests.h"

#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "TemporaryDirectory.h"
#include "TestAutomationQueue.h"
#include "TestNumeric.h"
#include "Worker.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace onda::plugin;

juce::String program(const juce::String &type, const juce::String &value,
                     const juce::String &range = "-1000000000.0, 1000000000.0",
                     const bool initializer = false) {
  return "params { p: " + type + " = " + value + " {" + range +
         "} }\n"
         "outs { out }\n"
         "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
         "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n" +
         (initializer ? "init { pinned = p }\nsample { out = f32(pinned) }\n"
                      : "sample { out = f32(p) }\n");
}

bool render(Processor &processor, const float expected,
            const float tolerance = 0.0F) {
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  audio.clear();
  juce::MidiBuffer midi;
  if (!test::processRealtime(processor, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (std::is_eq(tolerance <=> 0.0F)
            ? !std::is_eq(audio.getSample(0, frame) <=> expected)
            : !test::withinTolerance(audio.getSample(0, frame) - expected,
                                     tolerance)) {
      std::cerr << "ordinary parameter default: " << audio.getSample(0, frame)
                << ", expected " << expected << '\n';
      return false;
    }
  return true;
}

bool roundTrip(Processor &processor, const Product product,
               const float expected) {
  juce::MemoryBlock saved;
  processor.getStateInformation(saved);
  Processor restored(product);
  restored.setStateInformation(saved.getData(),
                               static_cast<int>(saved.getSize()));
  restored.prepareToPlay(48'000.0, 8);
  return render(restored, expected);
}

bool pendingDefaults(const Product product) {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "pending-parameter-default.onda")};
  if (!source.replaceWithText(program("f64", "1.0")))
    return false;
  Processor processor(product);
  processor.prepareToPlay(48'000.0, 8);
  processor.loadFile(source);
  const auto wait = [&](const std::uint64_t previous) {
    return test::waitUntil(
        [&] {
          const auto status = processor.workerStatus();
          return status.active && !status.compiling &&
                 status.engineGeneration != previous;
        },
        test::yieldToWorker);
  };
  if (!wait(0) || !render(processor, 1.0F) ||
      !roundTrip(processor, product, 1.0F))
    return false;
  const auto previous = processor.workerStatus().engineGeneration;
  if (!source.replaceWithText(program("f64", "2.0")))
    return false;
  processor.requestReload();
  if (!wait(previous) || !render(processor, 1.0F))
    return false;
  test::service(processor);
  return render(processor, 1.0F) && roundTrip(processor, product, 1.0F);
}

bool defaultEcho() {
  ParameterBank<1> values;
  ScalarValue value{0.5};
  value.scalarDefault = ScalarValue::ScalarDefault{
      1.0, {-1.0e30, 1.0e30, 0.0, 0.0, 0U, ScalarDomain::Type::f64}};
  values.setDefault(0, value);
  HostAutomation<1> automation{values};
  const std::array points{AutomationPoint{0, 0.5}};
  TestAutomationQueue queue{points};
  // Reset overlapping an editor ramp does not turn a host endpoint into
  // editor feedback, even if its normalized value is identical.
  automation.capture(0, queue.queue(), 1, 0, nullptr, true);
  automation.publish(0, false);
  return !values.target(0).defaulted;
}

bool reloadDuringSeedPublication() {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "reload-during-seed-publication.onda")};
  auto text = juce::String{"ins {\n"};
  if constexpr (pluginInputChannels > 0)
    text += "audio: f32[" + juce::String{pluginInputChannels} + "]\n";
  if constexpr (controlCount > 0)
    text += "cv = 1.0 {-1000000000.0, 1000000000.0}\n";
  text += "}\n" + program("f64", "1.0", "-1000000000.0, 1000000000.0", true);
  if (!source.replaceWithText(text))
    return false;
  ParameterState slots;
  ControlState controls;
  SpscSlot<PreparedEngine *> replacements, retirements;
  std::atomic<bool> deactivate{};
  Worker worker{
      Product::effect,
      replacements,
      retirements,
      deactivate,
      Worker::BuildFunction{},
      nullptr,
      [&] {
        HostParameterValues values;
        const auto snapshot = [](auto &bank, auto &destinations, auto &stamps) {
          for (std::size_t index = 0; index < destinations.size(); ++index) {
            const auto target = bank.preciseSnapshot(index);
            destinations[index] = target.value;
            stamps[index] = target.stamp;
          }
        };
        snapshot(slots, values.parameters, values.parameterStamps);
        snapshot(controls, values.controls, values.controlStamps);
        return values;
      }};
  worker.load(pathFromJuce(source.getFullPathName()), true);
  worker.configure(48'000.0, 8);
  static_cast<void>(worker.waitForPreparation());
  std::unique_ptr<PreparedEngine> first{replacements.tryPop()};
  if (!first || !first->parameterSeed())
    return false;
  worker.setActiveGeneration(first->buildGeneration());
  auto *seed = first->parameterSeed();
  seed->parameterStamps.before[0] = slots.snapshot(0).stamp;
  if constexpr (controlCount > 0)
    seed->controlStamps.before[0] = controls.snapshot(0).stamp;
  seed->started.store(true, std::memory_order_release);
  const auto claimed = worker.takeSeedValues();
  if (!claimed)
    return false;
  if (slots.seed(0, static_cast<float>(seed->values[0].normalized),
                 seed->parameterStamps.before[0],
                 seed->parameterStamps.after[0], true) == 0U)
    return false;
  if constexpr (controlCount > 0)
    if (controls.seed(0, static_cast<float>(seed->controls[0].normalized),
                      seed->controlStamps.before[0],
                      seed->controlStamps.after[0], true) == 0U)
      return false;
  // A worker snapshot after a seed write must already identify its exact
  // scalar, even while acknowledgment of the complete bank is pending.
  const auto snapshot = worker.hostParameterValues();
  worker.requestRebuild();
  static_cast<void>(worker.waitForPreparation());
  std::unique_ptr<PreparedEngine> replacement{replacements.tryPop()};
  worker.finishSeeding(claimed);
  if (!replacement)
    return false;
  const auto exact = snapshot.parameters[0].scalarDefault.has_value() &&
                     std::is_eq(snapshot.parameters[0].normalized <=>
                                seed->values[0].normalized);
  const auto exactControl =
      controlCount == 0 || (snapshot.controls[0].scalarDefault.has_value() &&
                            std::is_eq(snapshot.controls[0].normalized <=>
                                       seed->controls[0].normalized));
  juce::AudioBuffer<float> audio(pluginOutputChannels, 8);
  replacement->beginHostCallback(8);
  if (!replacement->process(nullptr, audio.getArrayOfWritePointers(), 8, {},
                            slots))
    return false;
  if (!exact || !exactControl || !std::is_eq(audio.getSample(0, 0) <=> 1.0F)) {
    std::cerr
        << "Reload during seed publication lost an exact default: initializer "
        << audio.getSample(0, 0) << ", expected 1\n";
    return false;
  }
  return true;
}

bool cachedResetDuringSeeding(const Product product) {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "cached-reset-during-seeding.onda")};
  constexpr auto wide = "-1000000000000000000000000000000.0, "
                        "1000000000000000000000000000000.0";
  if (!source.replaceWithText(program("f64", "1.0", wide)))
    return false;
  Processor processor(product);
  processor.prepareToPlay(48'000.0, 8);
  processor.loadFile(source);
  const auto wait = [&](const std::uint64_t previous) {
    return test::waitUntil(
        [&] {
          const auto status = processor.workerStatus();
          return status.active && !status.compiling &&
                 status.engineGeneration != previous;
        },
        test::yieldToWorker);
  };
  if (!wait(0) || !render(processor, 1.0F))
    return false;
  const auto previous = processor.workerStatus().engineGeneration;
  if (!source.replaceWithText(program("f64", "2.0", wide)))
    return false;
  processor.requestReload();
  if (!wait(previous) || !render(processor, 1.0F))
    return false;
  processor.resetParametersToDefaults();

  // Queue capture resolves Reset before rendering the inherited pending seed.
  // An empty queue warms the cache without adding a newer host endpoint.
  const TestAutomationQueue empty{std::span<const AutomationPoint>{}};
  const auto id = juce::VST3ClientExtensions::convertJuceParameterId("slot01");
  if (!test::captureRealtime(processor, id, empty.queue(), 8) ||
      !render(processor, 1.0F))
    return false;
  test::service(processor);
  if (!render(processor, 2.0F) || !render(processor, 2.0F)) {
    std::cerr << "pending seeding overwrote a cached Reset default\n";
    return false;
  }
  processor.getParameters()[0]->setValue(0.5F);
  if (!render(processor, 0.0F))
    return false;
  processor.resetParametersToDefaults();
  return render(processor, 2.0F) && roundTrip(processor, product, 2.0F);
}

bool logarithmicDefault(const Product product) {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "logarithmic-parameter-default.onda")};
  if (!source.replaceWithText(
          "params { p: f64 = 1.2345678912345 {0.001, 10000.0, log} }\n"
          "outs { out }\n"
          "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "init { pinned = p }\n"
          "sample { out = f32((p - pinned) * 1000000000000.0) }\n"))
    return false;
  Processor processor(product);
  processor.loadFile(source);
  processor.prepareToPlay(48'000.0, 8);
  if (!render(processor, 0.0F))
    return false;
  processor.getParameters()[0]->setValue(0.0F);
  processor.requestUserReset();
  if (!render(processor, 0.0F) || !roundTrip(processor, product, 0.0F))
    return false;
  processor.requestReload();
  processor.prepareToPlay(48'000.0, 8);
  return render(processor, 0.0F);
}

bool resetDuringBuild() {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "parameter-reset-during-build.onda")};
  if (!source.replaceWithText(program("f64", "1.0")))
    return false;
  ParameterState slots;
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
        for (std::size_t index = 0; index < slotCount; ++index) {
          const auto target = slots.preciseSnapshot(index);
          values.parameters[index] = target.value;
          values.parameterStamps[index] = target.stamp;
        }
        return values;
      }};
  worker.load(pathFromJuce(source.getFullPathName()), true);
  worker.configure(48'000.0, 8);
  static_cast<void>(worker.waitForPreparation());
  std::unique_ptr<PreparedEngine> first{replacements.tryPop()};
  if (!first || !first->parameterSeed())
    return false;
  worker.setActiveGeneration(first->buildGeneration());
  auto *seed = first->parameterSeed();
  seed->parameterStamps.before[0] = slots.snapshot(0).stamp;
  seed->started.store(true, std::memory_order_release);
  const auto claimed = worker.takeSeedValues();
  if (!claimed)
    return false;
  static_cast<void>(slots.seed(
      0, static_cast<float>(seed->values[0].normalized),
      seed->parameterStamps.before[0], seed->parameterStamps.after[0], true));
  worker.finishSeeding(claimed);
  gate.store(true, std::memory_order_release);
  if (!source.replaceWithText(program("f64", "2.0")))
    return false;
  worker.requestRebuild();
  const auto ready =
      test::waitUntil([&] { return entered.load(std::memory_order_acquire); },
                      test::yieldToWorker);
  if (ready) {
    ControlState controls;
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
  bool rendered{};
  if (!test::auditRealtime([&] {
        replacement->beginHostCallback(8);
        rendered = replacement->process(
            nullptr, audio.getArrayOfWritePointers(), 8, {}, slots);
      }) ||
      !rendered)
    return false;
  return std::is_eq(audio.getSample(0, 0) <=> 1.0F);
}
} // namespace

bool exerciseParameterDefaultPrecision() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "ordinary-parameter-default.onda")};
  const auto id = juce::VST3ClientExtensions::convertJuceParameterId("slot01");
  for (const auto product : {Product::effect, Product::instrument}) {
    for (const auto type : {"f32", "f64"}) {
      if (!source.replaceWithText(program(type, "1.0")))
        return false;
      Processor processor(product);
      processor.loadFile(source, true);
      processor.prepareToPlay(48'000.0, 8);
      if (!render(processor, 1.0F) || !render(processor, 1.0F) ||
          !roundTrip(processor, product, 1.0F))
        return false;

      // Equal normalized host writes and queues still replace scalar defaults.
      processor.getParameters()[0]->setValue(0.5F);
      if (!render(processor, 0.0F))
        return false;
      processor.resetParametersToDefaults();
      if (!render(processor, 1.0F))
        return false;
      const std::array points{AutomationPoint{0, 0.5}};
      TestAutomationQueue queue{points};
      if (!test::captureRealtime(processor, id, queue.queue(), 8) ||
          !render(processor, 0.0F) || !render(processor, 0.0F))
        return false;

      processor.requestUserReset();
      if (!render(processor, 1.0F) || !roundTrip(processor, product, 1.0F))
        return false;
      // Reload retains the old default's value even if its declaration changes.
      if (!source.replaceWithText(program(type, "2.0")))
        return false;
      processor.requestReload();
      processor.prepareToPlay(48'000.0, 8);
      if (!render(processor, 1.0F))
        return false;
      processor.resetParametersToDefaults();
      if (!render(processor, 2.0F) || !roundTrip(processor, product, 2.0F))
        return false;

      if (!source.replaceWithText(
              program(type, "7.0", "-2000000000.0, 2000000000.0")))
        return false;
      processor.requestReload();
      processor.prepareToPlay(48'000.0, 8);
      if (!render(processor, 4.0F, 1.0e-5F)) {
        std::cerr << "a changed parameter range reused the old plain default\n";
        return false;
      }

      // Defaults also survive ranges where double normalization itself loses
      // the scalar, and editor writes invalidate that identity before ramping.
      constexpr auto wide = "-1000000000000000000000000000000.0, "
                            "1000000000000000000000000000000.0";
      if (!source.replaceWithText(program(type, "1.0", wide)))
        return false;
      processor.loadFile(source, true);
      processor.prepareToPlay(48'000.0, 8);
      if (!render(processor, 1.0F))
        return false;
      processor.setSlotValue(0, 0.5F);
      if (!test::captureRealtime(processor, id, queue.queue(), 8))
        return false;
      if (!render(processor,
                  static_cast<float>(std::lerp(1.0, 0.0, 8.0 / 1440.0))))
        return false;
      processor.getParameters()[0]->setValue(0.5F);
      if (!render(processor, 0.0F))
        return false;
      processor.requestUserReset();
      if (!render(processor, 1.0F) || !roundTrip(processor, product, 1.0F))
        return false;

      // A reload's initializer must receive the exact retained default too.
      if (!source.replaceWithText(program(type, "7.0", wide, true)))
        return false;
      processor.requestReload();
      processor.prepareToPlay(48'000.0, 8);
      if (!render(processor, 1.0F))
        return false;
    }
    if (!pendingDefaults(product) || !cachedResetDuringSeeding(product) ||
        !logarithmicDefault(product))
      return false;
  }
  return defaultEcho() && reloadDuringSeedPublication() && resetDuringBuild();
}
