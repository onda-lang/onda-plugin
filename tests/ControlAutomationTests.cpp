#include "ControlAutomationTests.h"

#include "ControlInputTestSupport.h"
#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "TemporaryDirectory.h"
#include "TestAutomationQueue.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <vector>

namespace {
using namespace onda::plugin;

juce::String program() {
  auto source = "ins {\n" + test::mainAudioInputDeclaration() +
                "  modulation = -1.0 {-2.0, 2.0}\n";
  if constexpr (pluginControlInputs > 1)
    source += "  extra = 0.0\n";
  source += "}\nouts { out1";
  if constexpr (pluginOutputChannels > 1)
    source += ", out2";
  source += "}\ninit { blocks = 0; held = 1.0 }\n"
            "event note_on(id: i32, channel: i32, key: i32, velocity: f32) { "
            "held = 0.5 }\n"
            "event note_off(id: i32, channel: i32, key: i32, velocity: f32) { "
            "held = 1.0 }\n"
            "block { blocks += 1; sample { out1 = modulation * held\n";
  if constexpr (pluginOutputChannels > 1)
    source += "out2 = f32(blocks)\n";
  return source + "} }\n";
}

// Scan afresh for every sample and use a weighted sum in long double.
// This reference shares no cursor or interpolation arithmetic with production.
long double
referenceAutomationValue(const std::span<const AutomationPoint> points,
                         const double initial, const int offset,
                         const bool continuous) {
  AutomationPoint previous{-1, initial};
  for (const auto point : points) {
    if (point.offset <= offset) {
      previous = point;
      continue;
    }
    if (!continuous)
      return previous.value;
    const auto weight =
        static_cast<long double>(offset - previous.offset) /
        static_cast<long double>(point.offset - previous.offset);
    return (1.0L - weight) * previous.value + weight * point.value;
  }
  return previous.value;
}

bool checkCurveRendering(const std::span<const AutomationPoint> points,
                         const double initial, const int frames,
                         std::mt19937 &random) {
  std::vector<AutomationPoint> valid;
  for (auto point : points) {
    // Zero-sample flushes may still carry an endpoint at sample zero.
    if (point.offset < 0 || point.offset >= std::max(frames, 1) ||
        !std::isfinite(point.value))
      continue;
    point.value = std::clamp(point.value, 0.0, 1.0);
    valid.push_back(point);
  }
  const auto endpoint = valid.empty() ? std::optional<double>{}
                                      : std::optional{valid.back().value};
  TestAutomationQueue queue{points};
  AutomationCurve rendered, whole, continuous, discrete;
  for (auto *curve : {&rendered, &whole, &continuous, &discrete}) {
    curve->bind(queue.queue(), initial, frames);
    if (curve->finalValue() != endpoint) {
      std::cerr << "curve endpoint mismatch, frames=" << frames << '\n';
      return false;
    }
    curve->start(true);
  }
  discrete.start(false);
  std::vector<float> values(static_cast<std::size_t>(frames));
  std::vector<float> unsplit(values.size());
  whole.render(unsplit.data(), 0, frames, -200.0, 1200.0);
  for (int offset = 0; offset < frames;) {
    const auto count =
        std::min(frames - offset, static_cast<int>(random() % 11U) + 1);
    rendered.render(values.data() + offset, offset, count, -200.0, 1200.0);
    offset += count;
  }
  if (values != unsplit) {
    std::cerr << "curve differs across segment splits\n";
    return false;
  }
  for (int frame = 0; frame < frames; ++frame) {
    const auto expected = referenceAutomationValue(valid, initial, frame, true);
    const auto stepped = referenceAutomationValue(valid, initial, frame, false);
    const auto plain =
        static_cast<double>((1.0L - expected) * -200.0L + expected * 1200.0L);
    const auto tolerance = std::max(1.0, std::abs(plain)) * 2.0 *
                           std::numeric_limits<float>::epsilon();
    if (!test::withinTolerance(continuous.valueAt(frame) -
                                   static_cast<double>(expected),
                               1.0e-14) ||
        !std::is_eq(discrete.valueAt(frame) <=> static_cast<double>(stepped)) ||
        !test::withinTolerance(values[static_cast<std::size_t>(frame)] - plain,
                               tolerance)) {
      std::cerr << "curve reference mismatch, frames=" << frames
                << ", sample=" << frame << '\n';
      return false;
    }
  }
  return true;
}

bool curveRendering() {
  std::mt19937 random{781};
  const std::array cases{
      std::vector<AutomationPoint>{},
      std::vector{AutomationPoint{7, 0.75}},
      std::vector{AutomationPoint{7, 0.25}},
      std::vector{AutomationPoint{0, 0.25}, AutomationPoint{7, 0.25}},
      std::vector{AutomationPoint{3, 0.25}, AutomationPoint{4, 0.75}},
      std::vector{AutomationPoint{0, 0.1}, AutomationPoint{0, 0.4},
                  AutomationPoint{7, 0.9}, AutomationPoint{7, 0.6}},
      std::vector{AutomationPoint{-1, 0.9}, AutomationPoint{0, -2.0},
                  AutomationPoint{3, 2.0}, AutomationPoint{8, 0.5}},
      std::vector{
          AutomationPoint{0, std::numeric_limits<double>::quiet_NaN()},
          AutomationPoint{2, 0.5},
          AutomationPoint{3, std::numeric_limits<double>::infinity()},
          AutomationPoint{7, 0.75},
          AutomationPoint{7, -std::numeric_limits<double>::infinity()}}};
  for (const auto &points : cases)
    for (const auto frames : {0, 1, 8})
      if (!checkCurveRendering(points, 0.25, frames, random))
        return false;
  for (int trial = 0; trial < 2000; ++trial) {
    const auto frames = static_cast<int>(random() % 129U);
    std::array<AutomationPoint, 24> points{};
    for (auto &point : points) {
      point.offset =
          static_cast<int>(random() % static_cast<unsigned>(frames + 6)) - 3;
      point.value = static_cast<double>(random() % 1401U) / 1000.0 - 0.2;
    }
    std::ranges::stable_sort(points, {}, &AutomationPoint::offset);
    if (trial % 17 == 0)
      points[3].value = std::numeric_limits<double>::quiet_NaN();
    if (trial % 19 == 0)
      points[7].value = std::numeric_limits<double>::infinity();
    if (trial % 23 == 0)
      points[11].value = -std::numeric_limits<double>::infinity();
    const auto count =
        static_cast<std::size_t>(random() % (points.size() + 1U));
    const auto initial = static_cast<double>(random() % 1001U) / 1000.0;
    if (!checkCurveRendering(std::span{points}.first(count), initial, frames,
                             random)) {
      std::cerr << "random curve trial=" << trial << '\n';
      return false;
    }
  }
  ParameterBank<64> state;
  HostAutomation<64> automation{state};
  const std::array endpoint{AutomationPoint{0, 1.0}};
  TestAutomationQueue queue{endpoint};
  automation.capture(63, queue.queue(), 1, 7);
  automation.publish(7, true);
  automation.curves.clear();
  automation.publish(7, false);
  if (!std::is_eq(state.value(63) <=> 1.0) ||
      automation.takeNotifications() != (std::uint64_t{1} << 63U))
    return false;
  ParameterBank<0> empty;
  HostAutomation<0> noControls{empty};
  noControls.publish(0, false);
  if (noControls.takeNotifications() != 0)
    return false;

  // Exact double endpoints survive callback boundaries even when JUCE's
  // float representation does not change. An identical external write still
  // supersedes the older endpoint.
  ParameterBank<1> precise;
  HostAutomation<1> preciseAutomation{precise};
  const auto exact = std::nextafter(0.5, 1.0);
  const std::array precisePoint{AutomationPoint{0, exact}};
  TestAutomationQueue preciseQueue{precisePoint};
  preciseAutomation.capture(0, preciseQueue.queue(), 8, 0);
  preciseAutomation.publish(0, false);
  preciseAutomation.curves.clear();
  if (!std::is_eq(precise.target(0).value <=> exact))
    return false;
  const std::array following{AutomationPoint{7, 0.25}};
  TestAutomationQueue followingQueue{following};
  preciseAutomation.capture(0, followingQueue.queue(), 8, 0);
  preciseAutomation.curves.start(0, true);
  if (!std::is_eq(preciseAutomation.curves.valueAt(0, 0) <=>
                  std::lerp(exact, 0.25, 0.125)))
    return false;
  precise.set(0, 0.5);
  preciseAutomation.publish(0, false);
  return std::is_eq(precise.target(0).value <=> 0.5) &&
         !preciseAutomation.curves.active(0);
}

bool asymmetricRangeRendering() {
  for (const auto range :
       {std::array{-1.0e20, 1.0}, std::array{-1.0, 1.0e20}}) {
    for (const auto initial : {0.0, 1.0}) {
      const auto endpoint = 1.0 - initial;
      // Exercise both bulk fills and a span starting at the small endpoint.
      for (const auto points : {std::array{AutomationPoint{0, endpoint},
                                           AutomationPoint{7, endpoint}},
                                std::array{AutomationPoint{0, endpoint},
                                           AutomationPoint{7, initial}}}) {
        TestAutomationQueue queue{points};
        AutomationCurve whole, split, reference;
        for (auto *curve : {&whole, &split, &reference}) {
          curve->bind(queue.queue(), initial, 8);
          curve->start(true);
        }
        std::array<float, 8> output{}, segmented{};
        whole.render(output.data(), 0, 8, range[0], range[1]);
        split.render(segmented.data(), 0, 3, range[0], range[1]);
        split.render(segmented.data() + 3, 3, 5, range[0], range[1]);
        if (output != segmented)
          return false;
        for (int frame = 0; frame < 8; ++frame) {
          const auto expected = static_cast<float>(
              std::lerp(range[0], range[1], reference.valueAt(frame)));
          const auto actual = output[static_cast<std::size_t>(frame)];
          if (!test::withinTolerance(
                  actual - expected,
                  std::max(1.0F, std::abs(expected)) * 2.0F *
                      std::numeric_limits<float>::epsilon())) {
            std::cerr << "asymmetric control range sample " << frame << ": "
                      << actual << ", expected " << expected << '\n';
            return false;
          }
        }
      }
    }
  }
  return true;
}

bool bipolarRangeRendering() {
  // Exactly representable as f32: plain-unit slope arithmetic previously
  // rendered -16384 at the midpoint instead of zero.
  constexpr auto maximum = 282308283379207897088.0;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "bipolar-control-automation.onda")};
  if (!source.replaceWithText(
          "ins {\n" + test::mainAudioInputDeclaration() +
          "modulation = 0.0 {-282308283379207897088.0, "
          "282308283379207897088.0}\n}\nouts { out }\n"
          "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "sample { out = modulation }\n"))
    return false;
  const auto id = juce::VST3ClientExtensions::convertJuceParameterId("input01");
  for (const auto product : {Product::effect, Product::instrument}) {
    for (const auto block : {8, 3, 1}) {
      Processor processor(product);
      processor.prepareToPlay(48'000.0, 8);
      if (!processor.setPreferredBlockSize(block))
        return false;
      processor.loadFile(source);
      processor.prepareToPlay(48'000.0, 8);
      test::service(processor);
      const auto render = [&](const std::span<const AutomationPoint> points,
                              const std::span<const double> normalized,
                              const bool midiSplit = false) {
        const auto frames = static_cast<int>(normalized.size());
        TestAutomationQueue queue{points};
        juce::AudioBuffer<float> audio(
            std::max(pluginInputChannels, pluginOutputChannels), frames);
        audio.clear();
        juce::MidiBuffer midi;
        if (midiSplit)
          midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), 1);
        if (!test::captureRealtime(processor, id, queue.queue(), frames) ||
            !test::processRealtime(processor, audio, midi))
          return false;
        for (int frame = 0; frame < frames; ++frame) {
          const auto expected = static_cast<float>(std::lerp(
              -maximum, maximum, normalized[static_cast<std::size_t>(frame)]));
          const auto actual = audio.getSample(0, frame);
          if (!std::is_eq(actual <=> expected)) {
            std::cerr << "bipolar control block " << block << " sample "
                      << frame << ": " << actual << ", expected " << expected
                      << '\n';
            return false;
          }
        }
        return true;
      };
      const std::array points{AutomationPoint{0, 0.3}, AutomationPoint{2, 0.7}};
      const std::array normalized{0.3, 0.5, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7};
      if (!render(points, normalized) || !render(points, normalized, true))
        return false;
      // The next curve's implicit starting point is in the previous callback.
      const std::array first{AutomationPoint{0, 0.3}};
      const std::array initial{0.3};
      const std::array next{AutomationPoint{1, 0.7}};
      if (!render(first, initial) ||
          !render(next, std::span{normalized}.subspan(1)))
        return false;
    }
  }
  return true;
}

bool processorRendering(const juce::File &source, const int block) {
  Processor processor(Product::effect);
  processor.prepareToPlay(48'000.0, 8);
  if (!processor.setPreferredBlockSize(block)) {
    std::cerr << "block preference failed\n";
    return false;
  }
  processor.loadFile(source);
  processor.prepareToPlay(48'000.0, 8);
  test::service(processor);
  auto *parameter = processor.getParameters()[static_cast<int>(slotCount)];
  if (parameter->getName(128) != "modulation" || !parameter->isAutomatable() ||
      !test::withinTolerance(parameter->getDefaultValue() - 0.25F, 1.0e-6F) ||
      !test::withinTolerance(parameter->getValueForText("1.0") - 0.75F,
                             1.0e-6F) ||
      !test::withinTolerance(
          parameter->getText(0.75F, 128).getDoubleValue() - 1.0, 1.0e-6)) {
    std::cerr << "metadata failed: " << parameter->getName(128) << ": "
              << processor.workerStatus().message << "\n";
    return false;
  }
  if constexpr (pluginControlInputs > 1)
    if (processor.getParameters()[static_cast<int>(slotCount + 1)]->getName(
            128) != "extra")
      return false;

  const auto id = juce::VST3ClientExtensions::convertJuceParameterId("input01");
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  juce::MidiBuffer midi;
  int renderedBlocks{};
  test::BypassReference bypass;
  const auto render = [&](std::array<float, 8> expected,
                          const bool bypassed = false) {
    audio.clear();
    bypass.begin(bypassed, 8);
    if (!test::processRealtime(processor, audio, midi, bypassed))
      return false;
    for (int frame = 0; frame < 8; ++frame) {
      const auto held = midi.isEmpty() || frame < 3 ? 1.0F : 0.5F;
      const auto actual = audio.getSample(0, frame);
      const auto value =
          bypass.mix(expected[static_cast<std::size_t>(frame)] * held, 0.0F, frame);
      if (!test::withinTolerance(actual - value, 1.0e-6F)) {
        std::cerr << "control automation block " << block << " frame " << frame
                  << ": " << actual << " expected " << value << '\n';
        return false;
      }
      if constexpr (pluginOutputChannels > 1) {
        const auto blocks = bypass.mix(
            static_cast<float>(renderedBlocks + frame / block + 1), 0.0F, frame);
        if (!test::withinTolerance(audio.getSample(1, frame) - blocks, 1.0e-6F))
          return false;
      }
    }
    renderedBlocks += (8 + block - 1) / block;
    bypass.end(8);
    return true;
  };
  const std::array ramp{AutomationPoint{0, 0.0}, AutomationPoint{3, 1.0},
                        AutomationPoint{4, 0.25}, AutomationPoint{7, 0.75}};
  TestAutomationQueue queue{ramp};
  if (!test::captureRealtime(processor, id, queue.queue(), 8))
    return false;
  if (!render({-2, -2.0F / 3, 2.0F / 3, 2, -1, -1.0F / 3, 1.0F / 3, 1}))
    return false;
  const std::array down{AutomationPoint{7, 0.0}};
  TestAutomationQueue descending{down};
  midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), 3);
  if (!test::captureRealtime(processor, id, descending.queue(), 8))
    return false;
  if (!render({.625F, .25F, -.125F, -.5F, -.875F, -1.25F, -1.625F, -2}))
    return false;
  midi.clear();
  const std::array up{AutomationPoint{7, 1.0}};
  TestAutomationQueue ascending{up};
  if (!test::captureRealtime(processor, id, ascending.queue(), 8))
    return false;
  if (!render({-.75F, -.5F, -.25F, 0, .25F, .5F, .75F, 1}, true))
    return false;
  // Bypass still advances the control endpoint and Onda's logical blocks.
  if (!render({1, 1, 1, 1, 1, 1, 1, 1}))
    return false;

  // Store/restore the normalized endpoint, together with the embedded source.
  juce::MemoryBlock state;
  processor.getStateInformation(state);
  Processor restored(Product::effect);
  restored.setStateInformation(state.getData(),
                               static_cast<int>(state.getSize()));
  restored.prepareToPlay(48'000.0, 8);
  test::service(restored);
  if (!test::processRealtime(restored, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) - 2.0F, 1.0e-6F))
      return false;

  // A connected bus wins; its endpoint remains available when disconnected.
  auto layout = restored.getBusesLayout();
  layout.inputBuses.set(pluginMainInputBuses, juce::AudioChannelSet::mono());
  if (!restored.setBusesLayout(layout))
    return false;
  restored.prepareToPlay(48'000.0, 8);
  juce::AudioBuffer<float> routed(
      std::max(restored.getTotalNumInputChannels(), pluginOutputChannels), 8);
  routed.clear();
  std::fill_n(routed.getWritePointer(pluginInputChannels), 8, 0.4F);
  const auto priorReads = descending.readCount();
  if (!test::captureRealtime(restored, id, descending.queue(), 8))
    return false;
  if (!test::processRealtime(restored, routed, midi))
    return false;
  // Routed controls need only the endpoint; their curves are never started.
  if (descending.readCount() != priorReads + 1) {
    std::cerr << "routed control automation read discarded curve points\n";
    return false;
  }
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(routed.getSample(0, frame) - 0.4F, 1.0e-6F))
      return false;
  layout.inputBuses.set(pluginMainInputBuses,
                        juce::AudioChannelSet::disabled());
  if (!restored.setBusesLayout(layout))
    return false;
  restored.prepareToPlay(48'000.0, 8);
  if (!test::processRealtime(restored, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) + 2.0F, 1.0e-6F))
      return false;
  // Hot reloads preserve targets and refresh names even if params are
  // unchanged.
  auto *restoredControl = restored.getParameters()[static_cast<int>(slotCount)];
  restoredControl->setValueNotifyingHost(0.75F);
  const juce::File renamed{
      pathToJuce(testTemporaryRoot() / "control-renamed.onda")};
  if (!renamed.replaceWithText(program().replace("modulation", "renamed")))
    return false;
  restored.loadFile(renamed, false);
  restored.prepareToPlay(48'000.0, 8);
  test::service(restored);
  if (restoredControl->getName(128) != "renamed" ||
      !test::processRealtime(restored, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) - 1.0F, 1.0e-6F))
      return false;
  restored.requestUserReset();
  if (!test::processRealtime(restored, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) + 1.0F, 1.0e-6F))
      return false;

  return true;
}

bool seeding(const juce::File &source) {
  Processor processor(Product::effect);
  processor.prepareToPlay(48'000.0, 8);
  processor.loadFile(source);
  if (!test::waitUntil([&] { return processor.workerStatus().active; },
                       test::yieldToWorker))
    return false;
  const auto id = juce::VST3ClientExtensions::convertJuceParameterId("input01");
  const std::array points{AutomationPoint{7, 1.0}};
  TestAutomationQueue queue{points};
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  juce::MidiBuffer midi;
  if (!test::captureRealtime(processor, id, queue.queue(), 8))
    return false;
  if (!test::processRealtime(processor, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) + 1.0F, 1.0e-6F))
      return false;
  juce::MemoryBlock pending;
  processor.getStateInformation(pending);
  test::service(processor);
  const std::array descending{AutomationPoint{7, 0.0}};
  TestAutomationQueue down{descending};
  if (!test::captureRealtime(processor, id, down.queue(), 8))
    return false;
  if (!test::processRealtime(processor, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) -
                                   (1.5F - 0.5F * static_cast<float>(frame)),
                               1.0e-6F))
      return false;
  // A state saved before acknowledgement uses declared defaults.
  Processor restored(Product::effect);
  restored.setStateInformation(pending.getData(),
                               static_cast<int>(pending.getSize()));
  restored.prepareToPlay(48'000.0, 8);
  if (!test::processRealtime(restored, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) + 1.0F, 1.0e-6F))
      return false;

  // A direct control edit made while defaults are pending survives both the
  // seed commit and retained automation, and belongs in the saved checkpoint.
  const auto generation = processor.workerStatus().engineGeneration;
  processor.loadFile(source);
  if (!test::waitUntil(
          [&] {
            return processor.workerStatus().engineGeneration != generation;
          },
          test::yieldToWorker))
    return false;
  if (!test::captureRealtime(processor, id, queue.queue(), 8) ||
      !test::processRealtime(processor, audio, midi))
    return false;
  auto *control = processor.getParameters()[static_cast<int>(slotCount)];
  control->setValueNotifyingHost(0.6F);
  juce::MemoryBlock edited;
  processor.getStateInformation(edited);
  test::service(processor);
  if (!test::processRealtime(processor, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) - 0.4F, 1.0e-6F))
      return false;
  restored.setStateInformation(edited.getData(),
                               static_cast<int>(edited.getSize()));
  restored.prepareToPlay(48'000.0, 8);
  if (!test::processRealtime(restored, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) - 0.4F, 1.0e-6F))
      return false;
  return true;
}

bool constantRange() {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "constant-control-range.onda")};
  auto text = "ins {\n" + test::mainAudioInputDeclaration() +
              "  fixed = 1.0 {1.0, 1.0}\n}\nouts { out1 }\n"
              "sample { out1 = fixed }\n";
  if (!source.replaceWithText(text))
    return false;
  Processor processor(Product::effect);
  processor.loadFile(source);
  processor.prepareToPlay(48'000.0, 8);
  test::service(processor);
  auto *parameter = processor.getParameters()[static_cast<int>(slotCount)];
  for (const float normalized : {0.0F, 0.5F, 1.0F})
    if (!std::is_eq(parameter->getText(normalized, 128).getDoubleValue() <=>
                    1.0) ||
        parameter->getText(normalized, 128).containsIgnoreCase("nan"))
      return false;
  if (!std::is_eq(parameter->getValueForText("1.0") <=>
                  parameter->getDefaultValue()) ||
      !std::is_eq(parameter->getValueForText("99.0") <=>
                  parameter->getDefaultValue()))
    return false;
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  juce::MidiBuffer midi;
  return test::processRealtime(processor, audio, midi) &&
         std::all_of(audio.getReadPointer(0), audio.getReadPointer(0) + 8,
                     [](float value) { return std::is_eq(value <=> 1.0F); });
}

bool reloadPendingDefaults() {
  for (const bool changeRange : {false, true}) {
    const juce::File source{pathToJuce(
        testTemporaryRoot() / (changeRange ? "control-seed-range.onda"
                                           : "control-seed-default.onda"))};
    const auto program = [](const juce::String &value,
                            const juce::String &range) {
      auto text = "ins {\n" + test::mainAudioInputDeclaration() +
                  "  modulation = " + value + " {" + range + "}\n}\n";
      text += "params { level = " + value + " {" + range + "} }\n";
      text += "outs { out1";
      if constexpr (pluginOutputChannels > 1)
        text += ", out2";
      text += "}\nsample { out1 = modulation\n";
      if constexpr (pluginOutputChannels > 1)
        text += "out2 = level\n";
      return text + "}\n";
    };
    if (!source.replaceWithText(program("0.25", "0.0, 1.0")))
      return false;
    Processor processor(Product::effect);
    processor.prepareToPlay(48'000.0, 8);
    processor.loadFile(source);
    const auto waitForReplacement = [&](const std::uint64_t previous) {
      return test::waitUntil(
          [&] {
            const auto status = processor.workerStatus();
            return status.active && !status.compiling &&
                   status.engineGeneration != previous;
          },
          test::yieldToWorker);
    };
    juce::AudioBuffer<float> audio(
        std::max(pluginInputChannels, pluginOutputChannels), 8);
    juce::MidiBuffer midi;
    const auto render = [&](const float expected) {
      audio.clear();
      if (!test::processRealtime(processor, audio, midi))
        return false;
      for (int channel = 0; channel < std::min(pluginOutputChannels, 2);
           ++channel)
        for (int frame = 0; frame < 8; ++frame)
          if (!test::withinTolerance(audio.getSample(channel, frame) - expected,
                                     1.0e-6F)) {
            std::cerr << "pending control seed: channel " << channel
                      << " actual " << audio.getSample(channel, frame)
                      << " expected " << expected << '\n';
            return false;
          }
      return true;
    };
    // Adopt without a message-thread tick so the selection's seed remains
    // pending through the subsequent reload.
    if (!waitForReplacement(0) || !render(0.25F))
      return false;
    const auto previous = processor.workerStatus().engineGeneration;
    if (!source.replaceWithText(
            program("0.75", changeRange ? "-2.0, 2.0" : "0.0, 1.0")))
      return false;
    processor.requestReload();
    const auto expected = changeRange ? -1.0F : 0.25F;
    if (!waitForReplacement(previous) || !render(expected))
      return false;
    test::service(processor);
    if (!render(expected))
      return false;
  }
  return true;
}

bool undeclaredEndpoints() {
  const juce::File unused{
      pathToJuce(testTemporaryRoot() / "undeclared-control-automation.onda")};
  if (!unused.replaceWithText(test::unusedControlInputSource()))
    return false;
  Processor processor(Product::effect);
  processor.loadFile(unused);
  processor.prepareToPlay(48'000.0, 8);
  const auto last = controlCount - 1;
  const auto id = juce::VST3ClientExtensions::convertJuceParameterId(
      "input" + juce::String(static_cast<int>(last + 1)).paddedLeft('0', 2));
  struct Queue {
    mutable int reads{};
    static bool read(const void *context, const int index,
                     AutomationPoint &point) noexcept {
      auto &queue = *static_cast<const Queue *>(context);
      ++queue.reads;
      if (index < 0 || index >= 1024)
        return false;
      point = {index == 1023 ? 7 : 0, 0.875};
      return true;
    }
  } queue;
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    std::fill_n(audio.getWritePointer(channel), 8, 0.5F);
  juce::MidiBuffer midi;
  if (!test::captureRealtime(processor, id, {&queue, 1024, Queue::read}, 8) ||
      !test::processRealtime(processor, audio, midi) || queue.reads != 1)
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) - 0.5F, 1.0e-6F))
      return false;
  // An unused lane still retains its endpoint for a reload that declares it.
  const juce::File all{
      pathToJuce(testTemporaryRoot() / "declared-control-automation.onda")};
  if (!all.replaceWithText(test::allControlInputSource()))
    return false;
  processor.loadFile(all, false);
  processor.prepareToPlay(48'000.0, 8);
  if (!test::processRealtime(processor, audio, midi) || queue.reads != 1)
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) - 0.875F, 1.0e-6F))
      return false;
  return true;
}

bool allLaneAutomation() {
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "all-lane-automation.onda")};
  auto text = test::allControlInputSource();
  juce::String sum;
  for (int index = 0; index < pluginControlInputs; ++index) {
    if (index != 0)
      sum += " + ";
    sum += "cv" + juce::String(index + 1);
  }
  text = text.replace("out1 = cv" + juce::String(pluginControlInputs),
                      "out1 = " + sum);
  if (!source.replaceWithText(text))
    return false;
  Processor processor(Product::effect);
  processor.prepareToPlay(48'000.0, 8);
  if (!processor.setPreferredBlockSize(3))
    return false;
  processor.loadFile(source);
  processor.prepareToPlay(48'000.0, 8);
  std::array<std::array<AutomationPoint, 2>, controlCount> points{};
  std::vector<TestAutomationQueue> queues;
  queues.reserve(controlCount);
  for (std::size_t index = 0; index < controlCount; ++index) {
    points[index] = {
        AutomationPoint{0, 0.0},
        AutomationPoint{7, static_cast<double>(index + 1) /
                               static_cast<double>(controlCount + 1)}};
    queues.emplace_back(points[index]);
    const auto id = juce::VST3ClientExtensions::convertJuceParameterId(
        "input" + juce::String(static_cast<int>(index + 1)).paddedLeft('0', 2));
    if (!test::captureRealtime(processor, id, queues.back().queue(), 8))
      return false;
  }
  juce::AudioBuffer<float> audio(
      std::max(pluginInputChannels, pluginOutputChannels), 8);
  audio.clear();
  juce::MidiBuffer midi;
  if (!test::processRealtime(processor, audio, midi))
    return false;
  const auto endpoint = static_cast<float>(controlCount) / 2.0F;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) -
                                   endpoint * static_cast<float>(frame) / 7.0F,
                               1.0e-5F * static_cast<float>(controlCount)))
      return false;
  if (!test::processRealtime(processor, audio, midi))
    return false;
  for (int frame = 0; frame < 8; ++frame)
    if (!test::withinTolerance(audio.getSample(0, frame) - endpoint,
                               1.0e-5F * static_cast<float>(controlCount)))
      return false;
  return true;
}
} // namespace

bool exerciseNewControlDefaults() {
  using namespace onda::plugin;
  if constexpr (controlCount == 0)
    return true;
  enum class Origin { none, direct, queue };
  struct Write {
    Origin origin;
    float value;
  };
  const std::array writes{
      Write{Origin::none, 0.5F}, Write{Origin::direct, 0.5F},
      Write{Origin::direct, 0.875F}, Write{Origin::queue, 0.5F},
      Write{Origin::queue, 0.875F}};
  const auto check = [&](const Product product, const bool acknowledged,
                         const bool existing, const bool wide,
                         const bool lateWrite, const Write write) {
    const juce::File source{
        pathToJuce(testTemporaryRoot() / "new-control-default.onda")};
    const auto program = [&](const bool declared,
                             const juce::String &value = {}) {
      auto text = "ins {\n" + test::mainAudioInputDeclaration();
      if (existing)
        text += "  previous = 0.25 {0.0, 1.0}\n";
      if (declared)
        text +=
            "  cv = " + (value.isEmpty() ? (wide ? "1.0" : "0.125") : value) +
            (wide ? " {-1000000000000000000000000000000.0, "
                    "1000000000000000000000000000000.0}\n"
                  : " {0.0, 1.0}\n");
      text +=
          "}\nparams { level = 0.25 {0.0, 1.0} }\nouts { out }\n"
          "event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}\n"
          "event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}\n";
      return text + "sample { out = " + (declared ? "cv" : "0.0") +
             (existing ? " + previous" : "") + " }\n";
    };
    if (!source.replaceWithText(program(false)))
      return false;
    Processor processor(product);
    processor.prepareToPlay(48'000.0, 8);
    processor.loadFile(source);
    const auto wait = [&](const std::uint64_t previous) {
      const auto ready = test::waitUntil(
          [&] {
            const auto status = processor.workerStatus();
            return status.active && !status.compiling &&
                   status.engineGeneration != previous;
          },
          test::yieldToWorker);
      if (!ready)
        std::cerr << "new control preparation: "
                  << processor.workerStatus().message << '\n';
      return ready;
    };
    juce::AudioBuffer<float> audio(
        std::max(pluginInputChannels, pluginOutputChannels), 8);
    juce::MidiBuffer midi;
    const auto render = [&](Processor &instance, const float expected) {
      audio.clear();
      if (!test::processRealtime(instance, audio, midi))
        return false;
      for (int frame = 0; frame < 8; ++frame)
        if (!std::is_eq(audio.getSample(0, frame) <=> expected)) {
          std::cerr << "new control default: actual "
                    << audio.getSample(0, frame) << " expected " << expected
                    << " (acknowledged=" << acknowledged
                    << ", existing=" << existing << ", wide=" << wide
                    << ", lateWrite=" << lateWrite
                    << ", origin=" << static_cast<int>(write.origin) << ")\n";
          return false;
        }
      return true;
    };
    if (!wait(0) || !render(processor, existing ? 0.25F : 0.0F))
      return false;
    if (acknowledged) {
      test::service(processor);
      processor.getParameters()[0]->setValue(0.75F);
      if (existing)
        processor.getParameters()[static_cast<int>(slotCount)]->setValue(0.625F);
    }
    const auto previousValue =
        existing ? (acknowledged ? 0.625F : 0.25F) : 0.0F;
    const auto index = existing ? 1U : 0U;
    const auto id = juce::VST3ClientExtensions::convertJuceParameterId(
        existing ? "input02" : "input01");
    const std::array points{
        AutomationPoint{0, static_cast<double>(write.value)}};
    TestAutomationQueue queue{points};
    const auto applyWrite = [&] {
      if (write.origin == Origin::direct)
        processor.getParameters()[static_cast<int>(slotCount) + static_cast<int>(index)]->setValue(
            write.value);
      if (write.origin == Origin::queue)
        return test::captureRealtime(processor, id, queue.queue(), 8) &&
               queue.readCount() == 1;
      return true;
    };
    if (!lateWrite && (!applyWrite() || !render(processor, previousValue)))
      return false;
    const auto previous = processor.workerStatus().engineGeneration;
    if (!source.replaceWithText(program(true)))
      return false;
    processor.requestReload();
    if (!wait(previous) || (lateWrite && !applyWrite()))
      return false;
    const auto defaultPlain = wide ? 1.0F : 0.125F;
    const auto plain = write.origin == Origin::none
                           ? defaultPlain
                           : (wide ? 0.0F : write.value);
    if (!render(processor,
                previousValue + (acknowledged ? plain : defaultPlain)))
      return false;
    const auto roundTrip = [&] {
      juce::MemoryBlock saved;
      processor.getStateInformation(saved);
      Processor restored(product);
      restored.setStateInformation(saved.getData(),
                                   static_cast<int>(saved.getSize()));
      restored.prepareToPlay(48'000.0, 8);
      return render(restored, previousValue + plain);
    };
    if (write.origin == Origin::none && !roundTrip())
      return false;
    test::service(processor);
    const auto normalized =
        write.origin == Origin::none ? (wide ? 0.5F : 0.125F) : write.value;
    if (!render(processor, previousValue + plain) ||
        !std::is_eq(processor.getParameters()[static_cast<int>(slotCount) + static_cast<int>(index)]
                        ->getValue() <=> normalized) ||
        !std::is_eq(processor.getParameters()[0]->getValue() <=>
                    (acknowledged ? 0.75F : 0.25F)))
      return false;
    if (write.origin == Origin::none) {
      if (!roundTrip())
        return false;
      // A later reload retains the initialized exact default; a new patch
      // selection takes the new declaration's defaults instead.
      const auto generation = processor.workerStatus().engineGeneration;
      const auto replacement = wide ? "2.0" : "0.75";
      if (!source.replaceWithText(program(true, replacement)))
        return false;
      processor.requestReload();
      if (!wait(generation) || !render(processor, previousValue + plain))
        return false;
      processor.loadFile(source);
      processor.prepareToPlay(48'000.0, 8);
      if (!render(processor, (existing ? 0.25F : 0.0F) + (wide ? 2.0F : 0.75F)))
        return false;
    }
    return true;
  };
  for (const auto product : {Product::effect, Product::instrument}) {
    for (const bool acknowledged : {false, true}) {
      for (const bool existing : {false, true})
        for (const bool wide : {false, true})
          for (const bool lateWrite : {false, true})
            for (const auto write : writes) {
              if ((existing && controlCount < 2) ||
                  (wide && !std::is_eq(write.value <=> 0.5F)) ||
                  (lateWrite && write.origin == Origin::none))
                continue;
              if (!check(product, acknowledged, existing, wide, lateWrite,
                         write)) {
                std::cerr << "new control case failed: " << productName(product)
                          << ", acknowledged=" << acknowledged
                          << ", existing=" << existing << ", wide=" << wide
                          << ", lateWrite=" << lateWrite
                          << ", origin=" << static_cast<int>(write.origin)
                          << '\n';
                return false;
              }
            }
    }
  }
  return true;
}

bool exerciseControlAutomation() {
  if (!curveRendering()) {
    std::cerr << "curve rendering failed\n";
    return false;
  }
  if (!asymmetricRangeRendering())
    return false;
  if constexpr (onda::plugin::pluginControlInputs == 0)
    return true;
  if (!bipolarRangeRendering())
    return false;
  if (!constantRange()) {
    std::cerr << "constant control range conversion failed\n";
    return false;
  }
  const juce::File source{onda::plugin::pathToJuce(testTemporaryRoot() /
                                                   "control-automation.onda")};
  if (!source.replaceWithText(program()))
    return false;
  for (const int block : {8, 3, 1})
    if (!processorRendering(source, block)) {
      std::cerr << "processor control rendering failed block " << block << "\n";
      return false;
    }
  if (!seeding(source)) {
    std::cerr << "control seeding failed\n";
    return false;
  }
  if (!reloadPendingDefaults()) {
    std::cerr << "pending control defaults changed across reload\n";
    return false;
  }
  if (!undeclaredEndpoints() || !allLaneAutomation()) {
    std::cerr << "control lane capacity or unused endpoint retention failed\n";
    return false;
  }
  return true;
}
