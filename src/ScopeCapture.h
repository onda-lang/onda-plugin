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

struct ScopeRevision {
  std::uint64_t reset{};
  std::uint64_t frames{};
  bool enabled{};

  friend bool operator==(const ScopeRevision &,
                         const ScopeRevision &) = default;
};

class ScopeCapture final {
  static constexpr std::size_t outputChannels =
      static_cast<std::size_t>(pluginOutputChannels);

public:
  static constexpr std::size_t capacityFrames = 4096U;
  static constexpr std::size_t snapshotFrames = 1024U;

  void setEnabled(const bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_release);
    requestReset();
  }

  void requestReset() noexcept {
    requestedReset_.fetch_add(1U, std::memory_order_release);
  }

  void push(const std::array<float *, outputChannels> &channels,
            const int frames) noexcept {
    if (frames <= 0 || !enabled_.load(std::memory_order_acquire))
      return;

    const auto requestedReset = requestedReset_.load(std::memory_order_acquire);
    const auto reset =
        requestedReset != appliedReset_.load(std::memory_order_relaxed);
    const auto firstFrame =
        reset ? 0U : framesWritten_.load(std::memory_order_relaxed);
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
    if (reset)
      appliedReset_.store(requestedReset, std::memory_order_release);
  }

  [[nodiscard]] ScopeRevision revision() const noexcept {
    const auto enabled = enabled_.load(std::memory_order_acquire);
    const auto requestedReset = requestedReset_.load(std::memory_order_acquire);
    if (!enabled ||
        appliedReset_.load(std::memory_order_acquire) != requestedReset) {
      return {.reset = requestedReset, .enabled = enabled};
    }
    return {
        .reset = requestedReset,
        .frames = framesWritten_.load(std::memory_order_acquire),
        .enabled = true,
    };
  }

  [[nodiscard]] ScopeSnapshot snapshot() const {
    ScopeSnapshot result;
    if (!enabled_.load(std::memory_order_acquire))
      return result;
    const auto requestedReset = requestedReset_.load(std::memory_order_acquire);
    if (appliedReset_.load(std::memory_order_acquire) != requestedReset)
      return result;

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
  std::atomic<std::uint64_t> requestedReset_{};
  std::atomic<std::uint64_t> appliedReset_{};
  std::atomic<bool> enabled_{};
};

} // namespace onda::plugin
