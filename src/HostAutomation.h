#pragma once

#include "ParameterAutomation.h"

namespace onda::plugin {

template <std::size_t Count> class HostAutomation final {
public:
  explicit HostAutomation(ParameterBank<Count> &values) : values_(values) {}
  void capture(
      std::size_t index, AutomationQueue queue, int frames, std::uint64_t seed,
      const ParameterSeedStamps<Count> *stamps = nullptr,
      bool preserveEditorRamp = false,
      std::optional<ParameterValue::Target> observed = std::nullopt) noexcept {
    if (queue.count <= 0 || queue.read == nullptr)
      return;
    const auto target = observed ? *observed : values_.target(index);
    // Hosts return editor edits as queues too; preserve ordinary editor ramps.
    if (preserveEditorRamp && target.editor && !target.defaulted) {
      auto echo = true;
      for (int pointIndex = 0; pointIndex < queue.count && echo; ++pointIndex) {
        AutomationPoint point;
        echo = queue.read(queue.context, pointIndex, point) &&
               std::is_eq(point.value <=> target.value);
      }
      if (echo)
        return;
    }
    reconcileSeed(index, target, stamps);
    const auto initial =
        seed == seed_ && deferred_[index] &&
                deferred_[index]->expected.stamp == target.stamp
            ? deferred_[index]->value
            : target.value;
    curves.bind(index, queue, initial, frames);
    curves.setTargetStamp(index, target.stamp);
    captured_[index] = target;
  }
  void publish(std::uint64_t seed, bool useDefaults,
               const ParameterSeedStamps<Count> *stamps = nullptr) noexcept {
    if (seed != seed_) {
      detail::forEachAutomationLane(std::exchange(deferredLanes_, 0U),
                                    [this](const std::size_t index) noexcept {
                                      deferred_[index].reset();
                                    });
      seed_ = seed;
    }
    std::uint64_t notifications{};
    const auto pending =
        curves.boundLanes() | (useDefaults ? std::uint64_t{0} : deferredLanes_);
    detail::forEachAutomationLane(
        pending, [&](const std::size_t index) noexcept {
          const auto endpoint = curves.finalValue(index);
          if (useDefaults) {
            if (endpoint) {
              deferred_[index] = Deferred{*endpoint, captured_[index]};
              deferredLanes_ |= std::uint64_t{1} << index;
            }
          } else {
            if (endpoint || deferred_[index]) {
              if (deferred_[index])
                reconcileSeed(index, values_.snapshot(index), stamps);
              const auto observed =
                  endpoint ? captured_[index] : deferred_[index]->expected;
              const auto value = endpoint ? *endpoint : deferred_[index]->value;
              if (const auto committed =
                      values_.publish(index, value, observed)) {
                if (committed->stamp != observed.stamp)
                  notifications |= std::uint64_t{1} << index;
                captured_[index] = *committed;
                curves.setTargetStamp(index, committed->stamp);
              } else {
                curves.clear(index);
              }
            }
            deferred_[index].reset();
            deferredLanes_ &= ~(std::uint64_t{1} << index);
          }
        });
    if (notifications != 0U)
      notifications_.fetch_or(notifications, std::memory_order_release);
  }
  void notify(std::size_t index) noexcept {
    notifications_.fetch_or(std::uint64_t{1} << index,
                            std::memory_order_release);
  }
  std::uint64_t takeNotifications() noexcept {
    if (notifications_.load(std::memory_order_acquire) == 0U)
      return 0U;
    return notifications_.exchange(0U, std::memory_order_acq_rel);
  }
  // Deferred endpoints are writes even before they reach the parameter bank.
  std::uint64_t retainedLanes() const noexcept { return deferredLanes_; }
  AutomationBank<Count> curves;

private:
  using Target = typename ParameterBank<Count>::Target;
  struct Deferred {
    double value{};
    Target expected;
  };
  void reconcileSeed(std::size_t index, Target current,
                     const ParameterSeedStamps<Count> *stamps) noexcept {
    if (stamps != nullptr && deferred_[index] &&
        deferred_[index]->expected.stamp == stamps->before[index] &&
        current.stamp == stamps->after[index].load(std::memory_order_acquire)) {
      // A retained host endpoint supersedes a seeded exact default even when
      // both have the same JUCE float representation.
      current.defaulted = true;
      deferred_[index]->expected = current;
    }
  }
  static_assert(Count <= 64U);
  ParameterBank<Count> &values_;
  std::uint64_t seed_{};
  std::array<Target, Count> captured_{};
  std::array<std::optional<Deferred>, Count> deferred_{};
  std::uint64_t deferredLanes_{};
  std::atomic<std::uint64_t> notifications_{};
};

} // namespace onda::plugin
