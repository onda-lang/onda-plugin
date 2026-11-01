#pragma once

#include "Product.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace onda::plugin {

struct ScopeSnapshot {
  int channels{};
  std::vector<float> samples;
};

struct MeterSnapshot {
  static constexpr std::size_t count = pluginOutputChannels;
  std::array<float, count> peaks{};
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

  void setMeterEnabled(const bool enabled) noexcept {
    meterEnabled_.store(enabled, std::memory_order_release);
    clearMeter();
  }

  void clearMeter() noexcept {
    for (auto &peak : peaks_)
      peak.store(0.0f, std::memory_order_relaxed);
  }

  [[nodiscard]] MeterSnapshot meterSnapshot() noexcept {
    if (!meterEnabled_.load(std::memory_order_acquire))
      return {};
    for (std::size_t i = 0; i < lastMeter_.count; ++i) {
      // Negative means no new block; a captured zero is genuine silence.
      const auto peak = peaks_[i].exchange(-1.0f, std::memory_order_relaxed);
      if (peak >= 0.0f)
        lastMeter_.peaks[i] = peak;
    }
    return lastMeter_;
  }

  void requestReset() noexcept {
    clearMeter();
    requestedReset_.fetch_add(1U, std::memory_order_release);
  }

  void push(const std::array<float *, outputChannels> &channels,
            const int frames) noexcept {
    if (frames <= 0)
      return;
    if (meterEnabled_.load(std::memory_order_acquire)) {
      std::array<float, MeterSnapshot::count> blockPeaks{};
      for (std::size_t channel = 0; channel < outputChannels; ++channel) {
        auto &peak = blockPeaks[channel];
        for (int frame = 0; frame < frames; ++frame) {
          const auto sample = std::abs(channels[channel][frame]);
          if (std::isfinite(sample))
            peak = std::max(peak, sample);
        }
      }
      for (std::size_t i = 0; i < blockPeaks.size(); ++i) {
        auto previous = peaks_[i].load(std::memory_order_relaxed);
        while (previous < blockPeaks[i] &&
               !peaks_[i].compare_exchange_weak(previous, blockPeaks[i],
                                                std::memory_order_relaxed)) {
        }
      }
    }
    if (!enabled_.load(std::memory_order_acquire))
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
  std::atomic<bool> meterEnabled_{};
  std::array<std::atomic<float>, MeterSnapshot::count> peaks_{};
  MeterSnapshot lastMeter_{}; // Owned by the UI snapshot consumer.
};

} // namespace onda::plugin
