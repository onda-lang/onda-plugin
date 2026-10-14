#pragma once

#include "ParameterState.h"

#include <array>
#include <bit>
#include <optional>
#include <utility>

namespace onda::plugin {

namespace detail {
template <typename Visitor>
void forEachAutomationLane(std::uint64_t lanes, Visitor visitor) noexcept(
    noexcept(visitor(std::size_t{}))) {
  while (lanes != 0U) {
    visitor(static_cast<std::size_t>(std::countr_zero(lanes)));
    lanes &= lanes - 1U;
  }
}
} // namespace detail

struct AutomationPoint {
  int offset{};
  double value{};
};

// Borrowed for one host callback; reading points never allocates.
struct AutomationQueue {
  const void *context{};
  int count{};
  bool (*read)(const void *, int, AutomationPoint &) noexcept {};
};

// A monotonic cursor over one borrowed host queue. Ordinary parameters sample
// it at block boundaries; control inputs render it directly into the input
// slab.
class AutomationCurve final {
public:
  void bind(AutomationQueue queue, double initial, int frames) noexcept;
  void start(bool continuous) noexcept;
  bool active() const noexcept { return final_.has_value() && !constant_; }
  bool active(std::uint64_t stamp) const noexcept {
    return active() && (targetStamp_ == 0U || targetStamp_ == stamp);
  }
  void setTargetStamp(std::uint64_t stamp) noexcept { targetStamp_ = stamp; }
  double valueAt(int offset) noexcept;
  std::optional<double> finalValue() const noexcept { return final_; }
  // Render continuous samples with linear range mapping. Requires start(true)
  // for nonempty renders; offsets advance monotonically across calls.
  void render(float *destination, int offset, int frames, double minimum,
              double maximum) noexcept;

private:
  bool readPoint(int index, AutomationPoint &point) const noexcept;
  void readNext() noexcept;
  void advance(int offset) noexcept;
  AutomationQueue queue_{};
  double initial_{};
  AutomationPoint left_{-1, 0.0};
  AutomationPoint right_{};
  int next_{};
  int frames_{};
  bool continuous_{};
  bool constant_{};
  bool hasRight_{};
  double inverseSpan_{};
  std::optional<double> final_;
  std::uint64_t targetStamp_{};
};

template <std::size_t Count> class AutomationBank final {
public:
  void clear() noexcept {
    if (boundLanes_ == 0U)
      return;
    detail::forEachAutomationLane(
        std::exchange(boundLanes_, 0U),
        [this](const std::size_t index) noexcept { curves_[index] = {}; });
  }
  void clear(std::size_t index) noexcept {
    curves_[index] = {};
    boundLanes_ &= ~(std::uint64_t{1} << index);
  }
  void bind(std::size_t index, AutomationQueue queue, double initial,
            int frames) noexcept {
    curves_[index].bind(queue, initial, frames);
    boundLanes_ |= std::uint64_t{1} << index;
  }
  [[nodiscard]] std::uint64_t boundLanes() const noexcept {
    return boundLanes_;
  }
  void start(std::size_t index, bool continuous) noexcept {
    curves_[index].start(continuous);
  }
  bool active(std::size_t index) const noexcept {
    return curves_[index].active();
  }
  bool active(std::size_t index, std::uint64_t stamp) const noexcept {
    return curves_[index].active(stamp);
  }
  void setTargetStamp(std::size_t index, std::uint64_t stamp) noexcept {
    curves_[index].setTargetStamp(stamp);
  }
  double valueAt(std::size_t index, int offset) noexcept {
    return curves_[index].valueAt(offset);
  }
  std::optional<double> finalValue(std::size_t index) const noexcept {
    return curves_[index].finalValue();
  }
  void render(std::size_t index, float *destination, int offset, int frames,
              double minimum, double maximum) noexcept {
    curves_[index].render(destination, offset, frames, minimum, maximum);
  }

private:
  static_assert(Count <= 64U);
  std::array<AutomationCurve, Count> curves_{};
  std::uint64_t boundLanes_{};
};
using ParameterAutomation = AutomationBank<slotCount>;
using ControlAutomation = AutomationBank<controlCount>;

} // namespace onda::plugin
