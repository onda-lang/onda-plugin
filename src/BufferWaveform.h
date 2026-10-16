#pragma once

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace onda::plugin {

struct ProjectImage;
struct BufferSamples;

struct BufferWaveform {
  double minValue{};
  double maxValue{};
  std::vector<double> minimums;
  std::vector<double> maximums;
};

using BufferWaveformPtr = std::shared_ptr<const BufferWaveform>;
using BufferWaveforms = std::unordered_map<std::string, BufferWaveformPtr>;

// Called during preparation; no sample storage is retained by the preview.
[[nodiscard]] BufferWaveformPtr
makeBufferWaveform(std::span<const float> interleavedSamples, int channels);
[[nodiscard]] BufferWaveformPtr makeBufferWaveform(const BufferSamples &samples,
                                                   int channels);
[[nodiscard]] BufferWaveforms projectBufferWaveforms(const ProjectImage &image);

} // namespace onda::plugin
