#include "ScopeCapture.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>

int main() {
  using namespace onda::plugin;
  constexpr auto channels = static_cast<std::size_t>(pluginOutputChannels);
  auto capture = std::make_unique<ScopeCapture>();
  std::array<std::array<float, 4>, channels> samples{};
  std::array<float *, channels> pointers{};
  for (std::size_t channel = 0; channel < channels; ++channel) {
    const auto peak =
        static_cast<float>(channel + 1) / static_cast<float>(channels);
    samples[channel] = {peak * 0.5f, -peak,
                        std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity()};
    pointers[channel] = samples[channel].data();
  }
  capture->setMeterEnabled(true);
  capture->push(pointers, 4);
  for (auto &channel : samples)
    channel.fill(0.0f);
  capture->push(pointers, 4);
  const auto levels = capture->meterSnapshot();
  if (levels.peaks.size() != channels) {
    std::cerr << "Meter capture combined individual channels\n";
    return 1;
  }
  for (std::size_t channel = 0; channel < channels; ++channel) {
    const auto expected =
        static_cast<float>(channel + 1) / static_cast<float>(channels);
    if (std::abs(levels.peaks[channel] - expected) > 0.00001f) {
      std::cerr << "Meter capture lost a channel's transient peak\n";
      return 1;
    }
  }
  if (capture->revision().frames != 0 || !capture->snapshot().samples.empty()) {
    std::cerr << "Meter-only capture wrote waveform history\n";
    return 1;
  }
  const auto empty = capture->meterSnapshot();
  for (std::size_t channel = 0; channel < channels; ++channel) {
    if (empty.peaks[channel] != levels.peaks[channel]) {
      std::cerr << "Empty meter poll reported silence without a new audio block\n";
      return 1;
    }
  }
  capture->push(pointers, 4);
  const auto silence = capture->meterSnapshot();
  capture->setMeterEnabled(false);
  for (auto &channel : samples)
    channel.fill(1.0f);
  capture->push(pointers, 4);
  const auto disabled = capture->meterSnapshot();
  for (std::size_t channel = 0; channel < channels; ++channel) {
    if (silence.peaks[channel] != 0.0f || disabled.peaks[channel] != 0.0f) {
      std::cerr << "Silent or disabled meters retained peaks\n";
      return 1;
    }
  }
  capture->setMeterEnabled(true);
  capture->push(pointers, 4);
  static_cast<void>(capture->meterSnapshot());
  capture->requestReset();
  for (const auto peak : capture->meterSnapshot().peaks) {
    if (peak != 0.0f) {
      std::cerr << "Reset meter retained a previously displayed peak\n";
      return 1;
    }
  }
  std::cout << channels << "-channel meter capture passed\n";
}
