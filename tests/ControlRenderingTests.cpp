#include "ControlRenderingTests.h"
#include "ParameterAutomation.h"
#include "ParameterRamp.h"
#include "TestAutomationQueue.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

namespace {
using namespace onda::plugin;

bool sameSample(const float actual, const float expected) {
  return std::bit_cast<std::uint32_t>(actual) ==
         std::bit_cast<std::uint32_t>(expected);
}

bool rampEndpointPrecision() {
  // One sample below a long ramp's endpoint exposes fused/non-fused rounding
  // differences in real rendered output, rather than testing a lerp replica.
  constexpr auto duration = std::uint64_t{1} << 53U;
  for (const auto start : {1.0e20, -1.0e20}) {
    ParameterRamp batched;
    batched.setDuration(duration);
    batched.reset(start);
    batched.setTarget(std::copysign(1.0, start));
    static_cast<void>(batched.advance(duration - 2U, false));
    auto scalar = batched;
    std::array<float, 2> output;
    batched.render(output.data(), static_cast<int>(output.size()));
    for (const auto actual : output) {
      const auto expected = static_cast<float>(scalar.advance(1U, true));
      if (!sameSample(actual, expected)) {
        std::cerr << std::hexfloat << "Ramp endpoint mismatch: start " << start
                  << ", actual " << actual << ", expected " << expected
                  << std::defaultfloat << '\n';
        return false;
      }
    }
  }
  return true;
}

bool rampPrecision() {
  std::mt19937 random{97123};
  constexpr std::array<std::uint64_t, 10> durations{
      0,
      1,
      2,
      3,
      1440,
      4096,
      (std::uint64_t{1} << 53U) - 1U,
      (std::uint64_t{1} << 53U) + 1U,
      std::uint64_t{1} << 63U,
      std::numeric_limits<std::uint64_t>::max() - 1U};
  const auto randomValue = [&] {
    const auto value =
        std::bit_cast<float>(static_cast<std::uint32_t>(random()));
    return std::isfinite(value) ? static_cast<double>(value) : 0.0;
  };
  std::array<float, 257> actual{}, expected{}, segmented{};
  for (int trial = 0; trial < 2000; ++trial) {
    ParameterRamp batched, scalar;
    const auto initial = trial % 2 == 0 ? 0.1 : randomValue();
    batched.reset(initial);
    scalar.reset(initial);
    for (int update = 0; update < 8; ++update) {
      if (update % 3 == 0) {
        const auto duration = durations[random() % durations.size()];
        batched.setDuration(duration);
        scalar.setDuration(duration);
      }
      const auto target = randomValue();
      batched.setTarget(target);
      scalar.setTarget(target);
      if (update == 1) {
        // Exercise uint64 positions past double's exact integer range.
        constexpr auto skip = std::uint64_t{1} << 53U;
        static_cast<void>(batched.advance(skip, false));
        static_cast<void>(scalar.advance(skip, false));
      }
      auto split = batched;
      const auto frames = static_cast<int>(random() % actual.size());
      batched.render(nullptr, 0);
      batched.render(actual.data(), frames);
      for (int frame = 0; frame < frames; ++frame)
        expected[static_cast<std::size_t>(frame)] =
            static_cast<float>(scalar.advance(1U, true));
      for (int offset = 0; offset < frames;) {
        const auto count =
            std::min(frames - offset, static_cast<int>(random() % 17U) + 1);
        split.render(segmented.data() + offset, count);
        offset += count;
      }
      for (int frame = 0; frame < frames; ++frame) {
        const auto index = static_cast<std::size_t>(frame);
        if (!sameSample(actual[index], expected[index]) ||
            !sameSample(actual[index], segmented[index])) {
          std::cerr << "Batched ramp changed a sample: trial " << trial
                    << ", update " << update << ", frame " << frame << '\n';
          return false;
        }
      }
      // A subsequent retarget must retain the last rounded emitted value.
      const auto following = randomValue();
      for (auto *ramp : {&batched, &scalar, &split})
        ramp->setTarget(following);
      batched.render(actual.data(), 1);
      split.render(segmented.data(), 1);
      if (!sameSample(actual[0],
                      static_cast<float>(scalar.advance(1U, true))) ||
          !sameSample(actual[0], segmented[0]))
        return false;
    }
  }
  return true;
}

bool automationPrecision() {
  std::mt19937 random{8137};
  constexpr auto largest =
      static_cast<double>(std::numeric_limits<float>::max());
  constexpr auto smallest =
      static_cast<double>(std::numeric_limits<float>::denorm_min());
  const std::array ranges{
      std::array{-200.0, 1200.0},    std::array{0.0, 1.0},
      std::array{1.0, 1.0e20},       std::array{1.0e20, 1.0},
      std::array{-1.0e20, -1.0},     std::array{-1.0, -1.0e20},
      std::array{-1.0e20, 1.0},      std::array{-1.0, 1.0e20},
      std::array{-largest, largest}, std::array{smallest, 1.0},
      std::array{0.0, -0.0},         std::array{-0.0, -0.0},
      std::array{0.125, 0.125}};
  constexpr std::array values{0.0, -0.0, 0.3, 0.5, 0.7, 1.0};
  std::array<float, 129> actual{}, segmented{};
  for (int trial = 0; trial < 500; ++trial) {
    const auto frames = static_cast<int>(random() % actual.size());
    std::vector<AutomationPoint> points;
    const auto pointCount = static_cast<unsigned>(random() % 24U);
    for (unsigned i = 0; i < pointCount; ++i)
      points.push_back(
          {static_cast<int>(random() %
                            static_cast<unsigned>(std::max(frames, 1))),
           trial % 2 == 0 ? values[random() % values.size()]
                          : std::generate_canonical<double, 53>(random)});
    std::ranges::stable_sort(points, {}, &AutomationPoint::offset);
    const auto initial = values[random() % values.size()];
    for (const auto range : ranges) {
      TestAutomationQueue queue{points};
      AutomationCurve batched, scalar, split;
      for (auto *curve : {&batched, &scalar, &split}) {
        curve->bind(queue.queue(), initial, frames);
        curve->start(true);
      }
      batched.render(actual.data(), 0, frames, range[0], range[1]);
      for (int offset = 0; offset < frames;) {
        const auto count =
            std::min(frames - offset, static_cast<int>(random() % 17U) + 1);
        split.render(segmented.data() + offset, offset, count, range[0],
                     range[1]);
        offset += count;
      }
      for (int frame = 0; frame < frames; ++frame) {
        auto normalized = scalar.valueAt(frame);
        auto held = initial;
        auto constant = true;
        for (const auto point : points) {
          if (point.offset > frame) {
            constant = std::is_eq(point.value <=> held);
            break;
          }
          held = point.value;
        }
        // Bulk fills retain the held value, including its signed zero.
        if (constant || std::is_eq(range[0] <=> range[1]))
          normalized = held;
        const auto expected =
            static_cast<float>(std::lerp(range[0], range[1], normalized));
        const auto index = static_cast<std::size_t>(frame);
        if (!sameSample(actual[index], expected) ||
            !sameSample(actual[index], segmented[index])) {
          std::cerr << "Batched automation changed a sample: trial " << trial
                    << ", frame " << frame << ", range " << range[0] << ", "
                    << range[1] << ", normalized " << normalized
                    << std::hexfloat << ", actual " << actual[index]
                    << ", expected " << expected << ", split "
                    << segmented[index] << std::defaultfloat << '\n';
          return false;
        }
      }
    }
  }
  return true;
}
} // namespace

bool exerciseControlRendering() {
  return rampEndpointPrecision() && rampPrecision() && automationPrecision();
}
