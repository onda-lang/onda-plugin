#pragma once

#include <algorithm>
#include <compare>

namespace onda::plugin::detail {

// Finite endpoints and weights in [0, 1]. Select the arithmetic once per
// rendered span, preserving std::lerp's boundedness and exact endpoints.
template <typename Render>
void withLinearInterpolation(const double start, const double end,
                             Render render) noexcept {
  if ((start <= 0.0 && end >= 0.0) || (start >= 0.0 && end <= 0.0)) {
    render([=](const double weight) noexcept {
      // A weighted sum avoids overflow and cancellation across zero.
      return weight * end + (1.0 - weight) * start;
    });
  } else {
    const auto difference = end - start;
    if (end > start) {
      render([=](const double weight) noexcept {
        return std::is_eq(weight <=> 1.0)
                   ? end
                   : std::min(end, start + weight * difference);
      });
    } else {
      render([=](const double weight) noexcept {
        return std::is_eq(weight <=> 1.0)
                   ? end
                   : std::max(end, start + weight * difference);
      });
    }
  }
}

} // namespace onda::plugin::detail
