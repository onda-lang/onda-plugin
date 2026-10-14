#pragma once
#include "ParameterAutomation.h"
#include <span>

class TestAutomationQueue final {
public:
  explicit TestAutomationQueue(
      std::span<const onda::plugin::AutomationPoint> points)
      : points_(points) {}
  onda::plugin::AutomationQueue queue() const noexcept {
    return {this, static_cast<int>(points_.size()), read};
  }
  int readCount() const noexcept { return reads_; }

private:
  static bool read(const void *context, int index,
                   onda::plugin::AutomationPoint &point) noexcept {
    const auto &queue = *static_cast<const TestAutomationQueue *>(context);
    ++queue.reads_;
    const auto points = queue.points_;
    if (index < 0 || static_cast<std::size_t>(index) >= points.size())
      return false;
    point = points[static_cast<std::size_t>(index)];
    return true;
  }
  std::span<const onda::plugin::AutomationPoint> points_;
  mutable int reads_{};
};
