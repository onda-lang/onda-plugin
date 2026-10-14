#include "ParameterAutomation.h"
#include "LinearInterpolation.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace onda::plugin {

void AutomationCurve::bind(const AutomationQueue queue, const double initial,
                           const int frames) noexcept {
  *this = {};
  queue_ = queue;
  initial_ = initial;
  frames_ = frames;
  AutomationPoint point;
  for (auto index = queue_.count; index > 0; --index) {
    if (readPoint(index - 1, point)) {
      final_ = point.value;
      // The common stationary one-point queue needs no interpolation cursor.
      constant_ = queue_.count == 1 && point.value == initial_;
      break;
    }
  }
}

void AutomationCurve::start(const bool continuous) noexcept {
  left_ = {-1, initial_};
  next_ = 0;
  continuous_ = continuous;
  readNext();
}

bool AutomationCurve::readPoint(const int index,
                                AutomationPoint &point) const noexcept {
  if (queue_.read == nullptr || !queue_.read(queue_.context, index, point) ||
      point.offset < 0 || point.offset >= std::max(frames_, 1) ||
      !std::isfinite(point.value))
    return false;
  point.value = std::clamp(point.value, 0.0, 1.0);
  return true;
}

void AutomationCurve::readNext() noexcept {
  hasRight_ = false;
  while (next_ < queue_.count) {
    AutomationPoint point;
    if (!readPoint(next_++, point) || point.offset < left_.offset)
      continue;
    right_ = point;
    hasRight_ = true;
    const auto span = right_.offset - left_.offset;
    inverseSpan_ = span > 0 ? 1.0 / static_cast<double>(span) : 0.0;
    break;
  }
}

void AutomationCurve::advance(const int offset) noexcept {
  while (hasRight_ && right_.offset <= offset) {
    left_ = right_;
    readNext();
  }
}

double AutomationCurve::valueAt(const int offset) noexcept {
  advance(offset);
  if (!continuous_ || !hasRight_)
    return left_.value;
  const auto weight = static_cast<double>(offset - left_.offset) * inverseSpan_;
  return std::lerp(left_.value, right_.value, weight);
}

void AutomationCurve::render(float *destination, int offset, int frames,
                             const double minimum,
                             const double maximum) noexcept {
  if (frames <= 0)
    return;
  assert(continuous_);
  detail::withLinearInterpolation(
      minimum, maximum, [&](const auto scale) noexcept {
        while (frames > 0) {
          advance(offset);
          const auto count =
              hasRight_ ? std::min(frames, right_.offset - offset) : frames;
          if (!hasRight_ || left_.value == right_.value || minimum == maximum) {
            std::fill_n(destination, count,
                        static_cast<float>(scale(left_.value)));
          } else if (count == 1 && offset == left_.offset) {
            // An exact host point needs no span preparation. Keep the
            // zero-weight lerp to preserve its signed-zero arithmetic.
            destination[0] = static_cast<float>(
                scale(std::lerp(left_.value, right_.value, 0.0)));
          } else {
            // The reciprocal is cached once per pair of host points. Absolute
            // offsets give identical samples across segment splits.
            const auto firstOffset = offset - left_.offset;
            detail::withLinearInterpolation(
                left_.value, right_.value,
                [&](const auto interpolate) noexcept {
                  for (int frame = 0; frame < count; ++frame) {
                    const auto weight =
                        static_cast<double>(firstOffset + frame) * inverseSpan_;
                    // Interpolate before scaling to preserve wide bipolar
                    // zero crossings and small asymmetric endpoints.
                    destination[frame] =
                        static_cast<float>(scale(interpolate(weight)));
                  }
                });
          }
          destination += count;
          offset += count;
          frames -= count;
        }
      });
}

} // namespace onda::plugin
