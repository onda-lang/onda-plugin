#pragma once

#include <cmath>

namespace test {

// A NaN error must fail an audio/state comparison too. `abs(error) >= limit`
// alone silently accepts it because every ordered comparison with NaN is false.
inline bool withinTolerance(const double error, const double tolerance) {
  return std::isfinite(error) && std::abs(error) < tolerance;
}

} // namespace test
