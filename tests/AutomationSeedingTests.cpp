#include "AutomationSeedingTests.h"

#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "TemporaryDirectory.h"
#include "TestAutomationQueue.h"
#include "TestNumeric.h"

#include <array>
#include <iostream>

namespace {
class ValueListener final : public juce::AudioProcessorParameter::Listener {
public:
  explicit ValueListener(juce::AudioProcessorParameter &parameter)
      : parameter_(parameter) {
    parameter_.addListener(this);
  }
  ~ValueListener() override { parameter_.removeListener(this); }
  void parameterValueChanged(int, const float value) override { last = value; }
  void parameterGestureChanged(int, bool) override {}
  float last{};

private:
  juce::AudioProcessorParameter &parameter_;
};

// Inject a direct write when audio first reads an already published queue.
// This reproduces an edit between endpoint publication and DSP evaluation.
struct EditingQueue {
  onda::plugin::ParameterState &values;
  bool edit{};
  static bool read(const void *context, const int index,
                   onda::plugin::AutomationPoint &point) noexcept {
    auto &queue =
        *const_cast<EditingQueue *>(static_cast<const EditingQueue *>(context));
    if (index < 0 || index > 1)
      return false;
    if (index == 0 && queue.edit) {
      queue.values.set(0, 0.45F);
      queue.edit = false;
    }
    point = index == 0 ? onda::plugin::AutomationPoint{0, 0.8}
                       : onda::plugin::AutomationPoint{7, 0.6};
    return true;
  }
};
} // namespace

bool exerciseAutomationDuringSeeding() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "automation-during-seeding.onda")};
  if (!source.replaceWithText(R"(
outs { out1, out2 }
params { left = 0.25 {0.0, 1.0}; right = 0.75 {0.0, 1.0} }
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}
sample { out1 = left + right; out2 = left + right }
)"))
    return false;

  for (const auto product : {Product::effect, Product::instrument}) {
    Processor processor(product);
    processor.prepareToPlay(48'000.0, 8);
    processor.getParameters()[0]->setValueNotifyingHost(0.4F);
    processor.getParameters()[1]->setValueNotifyingHost(0.1F);
    ValueListener listener{*processor.getParameters()[0]};
    processor.loadFile(source, true);
    // Allow compilation and adoption without servicing the seeding timer.
    if (!test::waitUntil(
            [&] {
              const auto status = processor.workerStatus();
              return status.active && !status.compiling;
            },
            test::yieldToWorker))
      return false;

    juce::AudioBuffer<float> audio(2, 8);
    juce::MidiBuffer midi;
    const auto render = [&](const float expected) {
      if (!test::processRealtime(processor, audio, midi))
        return false;
      for (int channel = 0; channel < 2; ++channel)
        for (int frame = 0; frame < 8; ++frame)
          if (!test::withinTolerance(audio.getSample(channel, frame) - expected,
                                     1.0e-6F)) {
            std::cerr << "automation during seeding produced "
                      << audio.getSample(channel, frame) << ", expected "
                      << expected << '\n';
            return false;
          }
      return true;
    };
    const auto id =
        juce::VST3ClientExtensions::convertJuceParameterId("slot01");
    const std::array points{AutomationPoint{0, 0.4}};
    TestAutomationQueue queue{points};
    processor.captureHostAutomation(id, queue.queue(), 8);
    if (!render(1.0F))
      return false;
    // Later callbacks retain defaults until all host slots have been seeded.
    if (!render(1.0F))
      return false;
    test::service(processor);
    // The queued endpoint becomes authoritative after acknowledgment.
    if (!render(1.15F) ||
        !test::withinTolerance(processor.slotValue(0) - 0.4F, 1.0e-6F))
      return false;
    test::service(processor);
    if (!test::withinTolerance(listener.last - 0.4F, 1.0e-6F)) {
      std::cerr << "deferred endpoint did not reach host parameter listeners\n";
      return false;
    }
    processor.captureHostAutomation(id, queue.queue(), 8);
    if (!render(1.15F))
      return false;

    // Acknowledgment must preserve the endpoint as the next curve's implicit
    // -1 point, even when seeding overwrote the visible slot value.
    const auto generation = processor.workerStatus().engineGeneration;
    processor.loadFile(source, true);
    if (!test::waitUntil(
            [&] {
              const auto status = processor.workerStatus();
              return status.engineGeneration != generation && status.active &&
                     !status.compiling;
            },
            test::yieldToWorker))
      return false;
    const std::array latest{AutomationPoint{7, 0.6}};
    TestAutomationQueue latestQueue{latest};
    processor.captureHostAutomation(id, latestQueue.queue(), 8);
    if (!render(1.0F))
      return false;
    test::service(processor);
    const std::array falling{AutomationPoint{7, 0.0}};
    TestAutomationQueue fallingQueue{falling};
    processor.captureHostAutomation(id, fallingQueue.queue(), 8);
    if (!render(1.275F) || !render(0.75F))
      return false;

    // A new selection discards endpoints deferred for the superseded seed.
    const auto reloadDefaults = [&] {
      const auto prior = processor.workerStatus().engineGeneration;
      processor.loadFile(source, true);
      return test::waitUntil(
          [&] {
            const auto status = processor.workerStatus();
            return status.engineGeneration != prior && status.active &&
                   !status.compiling;
          },
          test::yieldToWorker);
    };
    if (!reloadDefaults())
      return false;
    processor.captureHostAutomation(id, queue.queue(), 8);
    if (!render(1.0F) || !reloadDefaults() || !render(1.0F))
      return false;
    test::service(processor);
    if (!render(1.0F))
      return false;

    // A newer editor target wins over an old deferred host endpoint.
    if (!reloadDefaults())
      return false;
    processor.captureHostAutomation(id, queue.queue(), 8);
    if (!render(1.0F))
      return false;
    test::service(processor);
    processor.setSlotValue(0, 0.9F);
    if (!render(1.0F + 0.65F * 8.0F / 1440.0F))
      return false;

    // New host curves start from a newer editor target rather than an endpoint
    // retained before the editor write.
    if (!reloadDefaults())
      return false;
    processor.captureHostAutomation(id, queue.queue(), 8);
    if (!render(1.0F))
      return false;
    test::service(processor);
    processor.setSlotValue(0, 0.9F);
    const std::array rising{AutomationPoint{7, 1.0}};
    TestAutomationQueue risingQueue{rising};
    processor.captureHostAutomation(id, risingQueue.queue(), 8);
    if (!render(1.6625F) || !render(1.75F))
      return false;

    // Edits made after adoption but before acknowledgment must survive seeding.
    if (!reloadDefaults())
      return false;
    processor.captureHostAutomation(id, queue.queue(), 8);
    if (!render(1.0F))
      return false;
    processor.setSlotValue(0, 0.9F);
    test::service(processor);
    if (!test::withinTolerance(processor.slotValue(0) - 0.9F, 1.0e-6F) ||
        !render(1.0F + 0.65F * 8.0F / 1440.0F)) {
      std::cerr << "seeding overwrote a newer editor target\n";
      return false;
    }

    // A newer direct host write also supersedes an older retained endpoint,
    // including a write that repeats the seed's value.
    for (const float value : {0.6F, 0.25F}) {
      if (!reloadDefaults())
        return false;
      processor.captureHostAutomation(id, latestQueue.queue(), 8);
      if (!render(1.0F))
        return false;
      test::service(processor);
      processor.getParameters()[0]->setValueNotifyingHost(value);
      if (!render(value + 0.75F) ||
          !test::withinTolerance(processor.slotValue(0) - value, 1.0e-6F)) {
        std::cerr << "retained automation overwrote a newer host target\n";
        return false;
      }
    }

    // A write arriving between queue capture and publication cancels that
    // borrowed curve rather than being replaced by its stale endpoint.
    processor.captureHostAutomation(id, latestQueue.queue(), 8);
    processor.getParameters()[0]->setValueNotifyingHost(0.45F);
    if (!render(1.2F))
      return false;

    // Acknowledgment after capture cannot activate a curve borrowed while
    // defaults were pending. Its endpoint becomes effective next callback.
    if (!reloadDefaults() || !render(1.0F))
      return false;
    processor.captureHostAutomation(id, latestQueue.queue(), 8);
    test::service(processor);
    if (!render(1.0F) || !render(1.35F))
      return false;
  }

  auto built = PreparedEngine::build(pathFromJuce(source.getFullPathName()),
                                     Product::effect, 48'000.0, 8);
  if (!built.engine)
    return false;
  ParameterState values;
  values.set(0, 0.25F);
  values.set(1, 0.75F);
  std::array<std::array<float, 8>, 2> audio{};
  const std::array outputs{audio[0].data(), audio[1].data()};
  built.engine->beginHostCallback(8);
  if (!built.engine->process(nullptr, outputs.data(), 8, {}, values))
    return false;
  HostAutomation<slotCount> automation{values};
  EditingQueue queue{values};
  automation.capture(0, {&queue, 2, EditingQueue::read}, 8, 0);
  automation.publish(0, false);
  queue.edit = true;
  if (!test::auditRealtime([&] {
        built.engine->beginHostCallback(8, &automation.curves);
        if (!built.engine->process(nullptr, outputs.data(), 8, {}, values))
          audio[0].fill(-1.0F);
      }))
    return false;
  for (const auto &channel : audio)
    for (const auto value : channel)
      if (!test::withinTolerance(value - 1.2F, 1.0e-6F)) {
        std::cerr << "borrowed automation replaced a newer direct write\n";
        return false;
      }
  return true;
}
