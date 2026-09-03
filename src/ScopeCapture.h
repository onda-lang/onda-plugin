#pragma once

#include "Product.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace onda::plugin {

struct ScopeSnapshot {
  int channels{};
  std::vector<float> samples;
};

class ScopeCapture final {
  static constexpr std::size_t outputChannels =
      static_cast<std::size_t>(pluginOutputChannels);

public:
  static constexpr std::size_t capacityFrames = 4096U;
  static constexpr std::size_t snapshotFrames = 1024U;

  void setEnabled(const bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_release);
    if (enabled)
      resetRequested_.store(true, std::memory_order_release);
  }

  void requestReset() noexcept {
    resetRequested_.store(true, std::memory_order_release);
  }

  void push(const std::array<float *, outputChannels> &channels,
            const int frames) noexcept {
    if (frames <= 0 || !enabled_.load(std::memory_order_acquire))
      return;
    if (resetRequested_.exchange(false, std::memory_order_acq_rel))
      framesWritten_.store(0U, std::memory_order_relaxed);

    const auto firstFrame = framesWritten_.load(std::memory_order_relaxed);
    for (auto frame = 0; frame < frames; ++frame) {
      const auto ringFrame = static_cast<std::size_t>(
          (firstFrame + static_cast<std::uint64_t>(frame)) % capacityFrames);
      for (std::size_t channel = 0; channel < outputChannels; ++channel) {
        samples_[ringFrame * outputChannels + channel].store(
            channels[channel][frame], std::memory_order_relaxed);
      }
    }
    framesWritten_.store(firstFrame + static_cast<std::uint64_t>(frames),
                         std::memory_order_release);
  }

  [[nodiscard]] ScopeSnapshot snapshot() const {
    ScopeSnapshot result;
    if (!enabled_.load(std::memory_order_acquire) ||
        resetRequested_.load(std::memory_order_acquire)) {
      return result;
    }

    const auto framesWritten = framesWritten_.load(std::memory_order_acquire);
    const auto frames = static_cast<std::size_t>(
        std::min<std::uint64_t>(framesWritten, snapshotFrames));
    result.channels = static_cast<int>(outputChannels);
    result.samples.reserve(frames * outputChannels);
    const auto firstFrame = framesWritten - frames;
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const auto ringFrame = static_cast<std::size_t>(
          (firstFrame + static_cast<std::uint64_t>(frame)) % capacityFrames);
      for (std::size_t channel = 0; channel < outputChannels; ++channel) {
        result.samples.push_back(
            samples_[ringFrame * outputChannels + channel].load(
                std::memory_order_relaxed));
      }
    }
    return result;
  }

private:
  static_assert(std::atomic<float>::is_always_lock_free);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

  std::array<std::atomic<float>, capacityFrames * outputChannels> samples_{};
  std::atomic<std::uint64_t> framesWritten_{};
  std::atomic<bool> enabled_{};
  std::atomic<bool> resetRequested_{};
};

} // namespace onda::plugin
