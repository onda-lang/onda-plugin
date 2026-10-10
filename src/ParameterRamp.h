#pragma once

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>

namespace onda::plugin {

inline constexpr double defaultParamSmoothingSeconds = 0.03;

[[nodiscard]] inline std::optional<std::uint64_t>
parameterSmoothingSamples(const double seconds,
                          const double sampleRate) noexcept {
  const auto count = std::ceil(seconds * sampleRate);
  if (!std::isfinite(seconds) || seconds < 0.0 || !std::isfinite(count) ||
      count >= static_cast<double>(std::numeric_limits<std::uint64_t>::max()))
    return std::nullopt;
  return static_cast<std::uint64_t>(count);
}

// Host-owned linear ramp in plain units, advanced once per audio block.
class ParameterRamp final {
public:
  void reset(const double value) noexcept {
    current_ = start_ = target_ = value;
    elapsed_ = samples_;
  }

  void setDuration(const std::uint64_t samples) noexcept {
    if (samples == samples_)
      return;
    const auto wasActive = active();
    samples_ = samples;
    if (samples == 0U)
      reset(target_);
    start_ = current_;
    elapsed_ = wasActive ? 0U : samples;
  }

  void setTarget(const double value) noexcept {
    if (samples_ == 0U || !std::isfinite(value) || !std::isfinite(current_)) {
      reset(value);
    } else if (!std::is_eq(value <=> target_)) {
      start_ = current_;
      target_ = value;
      elapsed_ = std::is_eq(value <=> current_) ? samples_ : 0U;
    }
  }

  [[nodiscard]] bool active() const noexcept { return elapsed_ < samples_; }

  [[nodiscard]] double advance(const std::uint64_t frames,
                               const bool singlePrecision) noexcept {
    if (active()) {
      elapsed_ += std::min(frames, samples_ - elapsed_);
      if (!active()) {
        current_ = target_;
      } else {
        const auto weight =
            static_cast<double>(elapsed_) / static_cast<double>(samples_);
        // Avoid overflow for finite endpoints, including opposite signs.
        current_ = std::lerp(start_, target_, weight);
      }
      if (singlePrecision)
        current_ = static_cast<double>(static_cast<float>(current_));
    }
    return current_;
  }

private:
  double current_{};
  double start_{};
  double target_{};
  std::uint64_t samples_{};
  std::uint64_t elapsed_{};
};

} // namespace onda::plugin
