#pragma once

#include "ControlValue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace onda::plugin {

inline constexpr std::size_t slotCount = 32U;
enum class ParameterReset { all, parameters, controls };
using ParameterValues = std::array<ScalarValue, slotCount>;

// The JUCE value, origin, default identity and sequence occupy one atomic word.
// Audio publishes exact endpoints through a stamped, lock-free cache.
class ParameterValue final {
public:
  // Revision one permanently identifies an untouched bank value.
  static constexpr std::uint64_t initialStamp =
      (std::uint64_t{1} << 32U) | std::bit_cast<std::uint32_t>(0.5F);

  struct Target {
    double value{};
    bool editor{};
    bool defaulted{};
    std::uint64_t stamp{};
  };

  std::uint64_t set(const float value, const bool editor = false,
                    const bool defaulted = false) noexcept {
    const auto normalized = normalize(value);
    auto previous = value_.load(std::memory_order_relaxed);
    auto committed = encode(normalized, editor, defaulted, previous);
    while (!value_.compare_exchange_weak(previous, committed,
                                         std::memory_order_release,
                                         std::memory_order_relaxed)) {
      committed = encode(normalized, editor, defaulted, previous);
    }
    return committed;
  }

  [[nodiscard]] Target snapshot() const noexcept {
    const auto bits = value_.load(std::memory_order_acquire);
    return {.value = static_cast<double>(std::bit_cast<float>(
                static_cast<std::uint32_t>(bits) & ~originBits)),
            .editor = (bits & editorBit) != 0U,
            .defaulted = (bits & defaultBit) != 0U,
            .stamp = bits};
  }

  // Audio only. A newer float write invalidates the cached exact endpoint,
  // even when it repeats the same value.
  [[nodiscard]] Target target() const noexcept {
    auto result = snapshot();
    if (result.stamp == preciseStamp_.load(std::memory_order_relaxed))
      result.value = preciseValue_.load(std::memory_order_relaxed);
    return result;
  }

  // Off-thread only. Retry if an endpoint publication or a newer float write
  // overlaps the read. Audio's target() never waits for another thread.
  [[nodiscard]] Target preciseSnapshot() const noexcept {
    for (;;) {
      const auto stamp = preciseStamp_.load(std::memory_order_acquire);
      if (stamp == publishingStamp)
        continue;
      auto result = snapshot();
      const auto precise = preciseValue_.load(std::memory_order_acquire);
      if (preciseStamp_.load(std::memory_order_acquire) != stamp ||
          value_.load(std::memory_order_acquire) != result.stamp)
        continue;
      if (result.stamp == stamp)
        result.value = precise;
      return result;
    }
  }

  [[nodiscard]] double value() const noexcept { return snapshot().value; }

  // Conditional default commit. Never replace a write made since adoption.
  [[nodiscard]] std::uint64_t seed(const float value,
                                   const std::uint64_t expected,
                                   std::atomic<std::uint64_t> &stamp,
                                   const bool defaulted = false) noexcept {
    return commit(normalize(value), expected, defaulted, &stamp);
  }

  [[nodiscard]] std::uint64_t
  initializeDefault(const float value,
                    std::atomic<std::uint64_t> &stamp) noexcept {
    return seed(value, initialStamp, stamp, true);
  }

  // Audio only. One CAS, with no retry, preserves newer editor/host writes.
  [[nodiscard]] std::optional<Target> publish(const double value,
                                              const Target observed) noexcept {
    const auto normalized = normalize(value);
    if (observed.stamp != initialStamp &&
        std::is_eq(normalized <=> observed.value) && !observed.editor &&
        !observed.defaulted)
      return value_.load(std::memory_order_acquire) == observed.stamp
                 ? std::optional{observed}
                 : std::nullopt;
    const auto previousStamp = preciseStamp_.load(std::memory_order_relaxed);
    // Readers cannot observe the committed float before its exact endpoint.
    preciseStamp_.store(publishingStamp, std::memory_order_release);
    const auto committed =
        commit(static_cast<float>(normalized), observed.stamp);
    if (committed == 0U) {
      preciseStamp_.store(previousStamp, std::memory_order_release);
      return std::nullopt;
    }
    // Acquire readers of a changed payload must also see the publishing
    // marker, preventing a new endpoint from being paired with an old stamp.
    preciseValue_.store(normalized, std::memory_order_release);
    preciseStamp_.store(committed, std::memory_order_release);
    return Target{.value = normalized, .stamp = committed};
  }

private:
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(std::atomic<double>::is_always_lock_free);
  static_assert(std::numeric_limits<float>::is_iec559);
  // Actual write stamps always have a nonzero revision in their upper word.
  static constexpr std::uint64_t publishingStamp = 1U;
  static constexpr std::uint32_t editorBit = std::uint32_t{1} << 31U;
  // Finite normalized floats occupy [0, 1], leaving this exponent bit unused.
  static constexpr std::uint32_t defaultBit = std::uint32_t{1} << 30U;
  static constexpr std::uint32_t originBits = editorBit | defaultBit;
  template <typename Value> static Value normalize(const Value value) noexcept {
    return std::isfinite(value) ? std::clamp(value, Value{0}, Value{1})
                                : Value{0.5};
  }
  static std::uint64_t encode(const float normalized, const bool editor,
                              const bool defaulted,
                              const std::uint64_t previous) noexcept {
    auto revision = static_cast<std::uint32_t>(previous >> 32U) + 1U;
    if (revision < 2U)
      revision = 2U;
    const auto bits = std::bit_cast<std::uint32_t>(normalized) & ~originBits;
    return (static_cast<std::uint64_t>(revision) << 32U) | bits |
           (editor ? editorBit : 0U) | (defaulted ? defaultBit : 0U);
  }
  std::uint64_t commit(const float normalized, std::uint64_t expected,
                       const bool defaulted = false,
                       std::atomic<std::uint64_t> *stamp = nullptr) noexcept {
    const auto committed = encode(normalized, false, defaulted, expected);
    // Publish default provenance before the value can become visible to saved
    // state readers. A failed conditional write cannot acquire that identity.
    if (stamp != nullptr)
      stamp->store(committed, std::memory_order_release);
    if (value_.compare_exchange_strong(expected, committed,
                                       std::memory_order_release,
                                       std::memory_order_relaxed))
      return committed;
    if (stamp != nullptr)
      stamp->store(0U, std::memory_order_release);
    return 0U;
  }
  std::atomic<std::uint64_t> value_{initialStamp};
  std::atomic<double> preciseValue_{};
  std::atomic<std::uint64_t> preciseStamp_{};
};

template <std::size_t Count> class ParameterBank {
public:
  using Target = ParameterValue::Target;
  void set(std::size_t index, float value, bool editor = false) noexcept {
    values_[index].set(value, editor);
  }
  Target target(std::size_t index) const noexcept {
    return values_[index].target();
  }
  Target snapshot(std::size_t index) const noexcept {
    return values_[index].snapshot();
  }
  Target preciseSnapshot(std::size_t index) const noexcept {
    return values_[index].preciseSnapshot();
  }
  double value(std::size_t index) const noexcept {
    return values_[index].value();
  }
  std::optional<Target> publish(std::size_t index, double value,
                                Target observed) noexcept {
    return values_[index].publish(value, observed);
  }
  std::uint64_t seed(std::size_t index, float value, std::uint64_t expected,
                     std::atomic<std::uint64_t> &stamp,
                     bool defaulted = false) noexcept {
    return values_[index].seed(value, expected, stamp, defaulted);
  }
  std::uint64_t initializeDefault(std::size_t index, float value,
                                  std::atomic<std::uint64_t> &stamp) noexcept {
    return values_[index].initializeDefault(value, stamp);
  }
  ParameterValue &operator[](std::size_t index) noexcept {
    return values_[index];
  }

  // Reset writers are serialized by Worker. Audio reads one coherent cache
  // snapshot or retains its previous complete value; it never waits.
  std::uint64_t setDefault(std::size_t index,
                           const ScalarValue &value) noexcept {
    assert(value.scalarDefault.has_value());
    auto &cached = defaults_[index];
    cached.revision.fetch_add(1U, std::memory_order_acq_rel);
    const auto stamp =
        values_[index].set(static_cast<float>(value.normalized), true, true);
    const auto &scalar = *value.scalarDefault;
    cached.normalized.store(value.normalized, std::memory_order_release);
    cached.plainBits.store(scalarPlainBits(scalar.plain),
                           std::memory_order_release);
    cached.plainIsInteger.store(
        std::holds_alternative<std::int64_t>(scalar.plain),
        std::memory_order_release);
    cached.minimum.store(scalar.domain.minimum, std::memory_order_release);
    cached.maximum.store(scalar.domain.maximum, std::memory_order_release);
    cached.step.store(scalar.domain.step, std::memory_order_release);
    cached.curve.store(scalar.domain.curve, std::memory_order_release);
    cached.stepCount.store(scalar.domain.stepCount, std::memory_order_release);
    cached.type.store(scalar.domain.type, std::memory_order_release);
    cached.logarithmic.store(scalar.domain.logarithmic,
                             std::memory_order_release);
    cached.hasCurve.store(scalar.domain.hasCurve, std::memory_order_release);
    cached.defaultMayExceedRange.store(scalar.domain.defaultMayExceedRange,
                                       std::memory_order_release);
    cached.stamp.store(stamp, std::memory_order_release);
    cached.revision.fetch_add(1U, std::memory_order_release);
    return stamp;
  }

  std::optional<ScalarValue> defaultValue(std::size_t index,
                                          std::uint64_t stamp) const noexcept {
    const auto &cached = defaults_[index];
    const auto revision = cached.revision.load(std::memory_order_acquire);
    if ((revision & 1U) != 0U ||
        cached.stamp.load(std::memory_order_acquire) != stamp)
      return std::nullopt;
    ScalarValue value{cached.normalized.load(std::memory_order_acquire)};
    value.scalarDefault = ScalarValue::ScalarDefault{
        scalarPlainFromBits(cached.plainBits.load(std::memory_order_acquire),
                            cached.plainIsInteger.load(std::memory_order_acquire)),
        {cached.minimum.load(std::memory_order_acquire),
         cached.maximum.load(std::memory_order_acquire),
         cached.step.load(std::memory_order_acquire),
         cached.curve.load(std::memory_order_acquire),
         cached.stepCount.load(std::memory_order_acquire),
         cached.type.load(std::memory_order_acquire),
         cached.logarithmic.load(std::memory_order_acquire),
         cached.hasCurve.load(std::memory_order_acquire),
         cached.defaultMayExceedRange.load(std::memory_order_acquire)}};
    return cached.revision.load(std::memory_order_acquire) == revision
               ? std::optional{value}
               : std::nullopt;
  }

private:
  static_assert(std::atomic<ScalarDomain::Type>::is_always_lock_free);
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
  static_assert(std::atomic<bool>::is_always_lock_free);
  struct Default {
    std::atomic<std::uint64_t> revision{}, stamp{}, plainBits{};
    std::atomic<double> normalized{}, minimum{}, maximum{}, step{},
        curve{};
    std::atomic<std::uint32_t> stepCount{};
    std::atomic<ScalarDomain::Type> type{};
    std::atomic<bool> logarithmic{}, hasCurve{}, defaultMayExceedRange{},
        plainIsInteger{};
  };
  std::array<ParameterValue, Count> values_;
  std::array<Default, Count> defaults_;
};

using ParameterState = ParameterBank<slotCount>;
using ControlDefaultStamps =
    std::array<std::atomic<std::uint64_t>, controlCount>;

struct HostParameterValues {
  ParameterValues parameters{};
  ControlValues controls{};
  std::array<std::uint64_t, slotCount> parameterStamps{};
  std::array<std::uint64_t, controlCount> controlStamps{};
};

// Adoption snapshots are immutable once started is published. Acknowledgment
// records which seed writes succeeded so deferred curves can follow them.
template <std::size_t Count> struct ParameterSeedStamps {
  std::array<std::uint64_t, Count> before{};
  std::array<std::atomic<std::uint64_t>, Count> after{};
};

} // namespace onda::plugin
