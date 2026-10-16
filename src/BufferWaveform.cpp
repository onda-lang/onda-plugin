#include "BufferWaveform.h"

#include "AudioFile.h"

#include "JucePath.h"
#include "OndaSdk.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace onda::plugin {
namespace {

template <typename Sample>
BufferWaveformPtr summarize(std::size_t frames, std::size_t channels,
                            Sample sample) {
  if (frames == 0U || channels == 0U)
    return {};
  auto result = std::make_shared<BufferWaveform>();
  const auto columns = std::min(frames, std::size_t{128});
  result->minimums.reserve(columns);
  result->maximums.reserve(columns);
  result->minValue = std::numeric_limits<double>::infinity();
  result->maxValue = -result->minValue;
  const auto frameAt = [=](std::size_t column) {
    return frames / columns * column + frames % columns * column / columns;
  };
  for (std::size_t column = 0; column < columns; ++column) {
    auto low = std::numeric_limits<double>::infinity();
    auto high = -low;
    const auto end = frameAt(column + 1U) * channels;
    for (auto index = frameAt(column) * channels; index < end; ++index) {
      const auto value = sample(index);
      if (std::isfinite(value)) {
        low = std::min(low, value);
        high = std::max(high, value);
      }
    }
    if (!std::isfinite(low))
      low = high = 0.0;
    result->minimums.push_back(low);
    result->maximums.push_back(high);
    result->minValue = std::min(result->minValue, low);
    result->maxValue = std::max(result->maxValue, high);
  }
  return result;
}

template <typename T>
BufferWaveformPtr typedWaveform(std::span<const std::uint8_t> samples,
                                const onda_buffer_asset_info_t &info) {
  if (samples.size() / sizeof(T) / info.channels != info.frames)
    return {};
  return summarize(info.frames, info.channels, [samples](std::size_t index) {
    T value{};
    std::memcpy(&value, samples.data() + index * sizeof(T), sizeof(T));
    return static_cast<double>(value);
  });
}

BufferWaveformPtr assetWaveform(std::span<const std::uint8_t> encoded) {
  onda_buffer_asset_info_t info{};
  OndaDiagnostic diagnostic;
  const auto size =
      onda_buffer_asset_decode(encoded.data(), encoded.size(), &info, nullptr,
                               0, diagnostic.outParameter());
  if (size <= 0 || info.channels == 0U ||
      static_cast<std::uint64_t>(size) > std::vector<std::uint8_t>{}.max_size())
    return {};
  std::vector<std::uint8_t> samples(static_cast<std::size_t>(size));
  if (onda_buffer_asset_decode(encoded.data(), encoded.size(), &info,
                               samples.data(), samples.size(),
                               diagnostic.outParameter()) != size)
    return {};
  switch (info.element_type) {
  case ONDA_PRIMITIVE_BOOL:
    return typedWaveform<std::uint8_t>(samples, info);
  case ONDA_PRIMITIVE_I32:
    return typedWaveform<std::int32_t>(samples, info);
  case ONDA_PRIMITIVE_I64:
    return typedWaveform<std::int64_t>(samples, info);
  case ONDA_PRIMITIVE_F32:
    return typedWaveform<float>(samples, info);
  case ONDA_PRIMITIVE_F64:
    return typedWaveform<double>(samples, info);
  default:
    return {};
  }
}

} // namespace

BufferWaveformPtr makeBufferWaveform(std::span<const float> samples,
                                     const int channels) {
  if (channels <= 0 ||
      samples.size() % static_cast<std::size_t>(channels) != 0U)
    return {};
  return summarize(samples.size() / static_cast<std::size_t>(channels),
                   static_cast<std::size_t>(channels),
                   [samples](std::size_t index) {
                     return static_cast<double>(samples[index]);
                   });
}

BufferWaveformPtr makeBufferWaveform(const BufferSamples &samples,
                                     const int channels) {
  return std::visit(
      [channels](const auto &values) -> BufferWaveformPtr {
        if (channels <= 0 ||
            values.size() % static_cast<std::size_t>(channels) != 0U)
          return {};
        return summarize(values.size() / static_cast<std::size_t>(channels),
                         static_cast<std::size_t>(channels),
                         [&values](std::size_t index) {
                           return static_cast<double>(values[index]);
                         });
      },
      samples.values);
}

BufferWaveforms projectBufferWaveforms(const ProjectImage &image) {
  // The release SDK exposes project samples through its editable export plan.
  // Read that plan in memory; never reopen external source/audio files here.
  std::map<std::filesystem::path, BufferWaveformPtr> assets;
  juce::var manifest;
  Diagnostic diagnostic;
  const auto visited = visitMaterializedProjectFiles(
      image, diagnostic, [&](MaterializedProjectFile &&file) {
        if (file.relativePath == "project.ondaproject") {
          manifest = juce::JSON::parse(juce::String::fromUTF8(
              reinterpret_cast<const char *>(file.bytes.data()),
              static_cast<int>(file.bytes.size())));
        } else if (file.relativePath.extension() == ".ondabuffer") {
          assets.emplace(file.relativePath, assetWaveform(file.bytes));
        }
        return true;
      });
  BufferWaveforms result;
  const auto bindings = manifest.getProperty("buffers", {});
  if (!visited || bindings.getDynamicObject() == nullptr)
    return result;
  const auto add = [&](const juce::String &name, const juce::var &binding) {
    const auto asset =
        assets.find(pathFromJuce(binding.getProperty("file", {}).toString()));
    if (asset != assets.end() && asset->second)
      result.emplace(name.toStdString(), asset->second);
  };
  for (const auto &property : bindings.getDynamicObject()->getProperties()) {
    const auto name = property.name.toString();
    if (const auto *elements = property.value.getArray()) {
      for (int index = 0; index < elements->size(); ++index)
        add(name + "[" + juce::String(index) + "]",
            elements->getReference(index));
    } else {
      add(name, property.value);
    }
  }
  return result;
}

} // namespace onda::plugin
