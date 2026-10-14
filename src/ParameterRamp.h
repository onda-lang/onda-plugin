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

// Plain-unit editor transitions advanced by actual rendered samples.
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

  void render(float *destination, const int frames) noexcept {
    if (frames <= 0)
      return;
    const auto remaining = samples_ - elapsed_;
    const auto count = static_cast<int>(
        std::min(static_cast<std::uint64_t>(frames), remaining));
    if (count > 0) {
      const auto finishes = static_cast<std::uint64_t>(count) == remaining;
      const auto interpolated = count - (finishes ? 1 : 0);
      const auto duration = static_cast<double>(samples_);
      for (int frame = 0; frame < interpolated; ++frame) {
        const auto position =
            elapsed_ + static_cast<std::uint64_t>(frame) + 1U;
        // Match scalar advance(), including the standard library's fused
        // arithmetic. Absolute positions keep segment sizes from affecting it.
        const auto weight = static_cast<double>(position) / duration;
        destination[frame] =
            static_cast<float>(std::lerp(start_, target_, weight));
      }
      if (finishes)
        destination[count - 1] = static_cast<float>(target_);
      elapsed_ += static_cast<std::uint64_t>(count);
      current_ = static_cast<double>(destination[count - 1]);
    }
    std::fill_n(destination + count, frames - count,
                static_cast<float>(current_));
  }

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
