#include "AudioFile.h"

#include "JucePath.h"
#include "OndaSdk.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <fstream>
#include <limits>
#include <memory>

namespace onda::plugin {
namespace {

void registerAudioFormats(juce::AudioFormatManager &formats) {
  formats.registerBasicFormats();
}

AudioFileResult failure(const std::string &message,
                        const std::filesystem::path &path) {
  AudioFileResult result;
  result.error = message + ": " + pathToJuce(path).toStdString();
  return result;
}

AudioFileResult decode(const std::vector<std::uint8_t> &encodedBytes,
                       const std::filesystem::path &displayPath) {
  if (encodedBytes.empty())
    return failure("Audio file contains no encoded data", displayPath);

  onda_buffer_asset_info_t assetInfo{};
  OndaDiagnostic assetDiagnostic;
  const auto decodedBytes = onda_buffer_asset_decode(
      encodedBytes.data(), encodedBytes.size(), &assetInfo, nullptr, 0,
      assetDiagnostic.outParameter());
  if (decodedBytes >= 0) {
    if (assetInfo.element_type != ONDA_PRIMITIVE_F32 ||
        assetInfo.frames == 0U || assetInfo.channels == 0U ||
        assetInfo.frames >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        assetInfo.channels >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        decodedBytes != static_cast<std::int64_t>(assetInfo.sample_bytes) ||
        assetInfo.sample_bytes % sizeof(float) != 0U) {
      return failure("Unsupported Onda buffer asset", displayPath);
    }
    DecodedAudioFile decoded{
        .interleavedSamples =
            std::vector<float>(assetInfo.sample_bytes / sizeof(float)),
        .frames = static_cast<int>(assetInfo.frames),
        .channels = static_cast<int>(assetInfo.channels),
        .sampleRate = assetInfo.sample_rate,
    };
    const auto written = onda_buffer_asset_decode(
        encodedBytes.data(), encodedBytes.size(), &assetInfo,
        decoded.interleavedSamples.data(), assetInfo.sample_bytes,
        assetDiagnostic.outParameter());
    if (written != decodedBytes)
      return failure("Could not decode the Onda buffer asset", displayPath);
    AudioFileResult result;
    result.audio = std::move(decoded);
    return result;
  }

  juce::AudioFormatManager formats;
  registerAudioFormats(formats);
  std::unique_ptr<juce::AudioFormatReader> reader{
      formats.createReaderFor(std::make_unique<juce::MemoryInputStream>(
          encodedBytes.data(), encodedBytes.size(), false))};
  if (!reader)
    return failure("Unsupported or unreadable audio file", displayPath);

  if (reader->lengthInSamples <= 0 || reader->numChannels == 0U)
    return failure("Audio file contains no samples", displayPath);
  if (reader->lengthInSamples > std::numeric_limits<int>::max() ||
      reader->numChannels >
          static_cast<unsigned int>(std::numeric_limits<int>::max()) ||
      !std::isfinite(reader->sampleRate) || reader->sampleRate <= 0.0 ||
      reader->sampleRate >
          static_cast<double>(std::numeric_limits<float>::max())) {
    return failure("Audio file dimensions are unsupported", displayPath);
  }

  const auto frames = static_cast<int>(reader->lengthInSamples);
  const auto channels = static_cast<int>(reader->numChannels);
  const auto sampleCount =
      static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);
  if (sampleCount > std::vector<float>{}.max_size())
    return failure("Audio file is too large", displayPath);

  juce::AudioBuffer<float> planar(channels, frames);
  if (!reader->read(planar.getArrayOfWritePointers(), channels, 0, frames))
    return failure("Failed to decode audio file", displayPath);

  DecodedAudioFile decoded{
      .interleavedSamples = std::vector<float>(sampleCount),
      .frames = frames,
      .channels = channels,
      .sampleRate = static_cast<float>(reader->sampleRate),
  };
  for (int frame = 0; frame < frames; ++frame) {
    for (int channel = 0; channel < channels; ++channel) {
      decoded.interleavedSamples[static_cast<std::size_t>(frame) *
                                     static_cast<std::size_t>(channels) +
                                 static_cast<std::size_t>(channel)] =
          planar.getSample(channel, frame);
    }
  }
  AudioFileResult result;
  result.audio = std::move(decoded);
  return result;
}

} // namespace

AudioFileResult decodeAudioFile(const std::filesystem::path &path) {
  std::error_code error;
  const auto fileBytes = std::filesystem::file_size(path, error);
  if (error || fileBytes == 0U ||
      fileBytes > std::vector<std::uint8_t>{}.max_size() ||
      fileBytes > static_cast<std::uintmax_t>(
                      std::numeric_limits<std::streamsize>::max())) {
    return failure("Unsupported or unreadable audio file", path);
  }
  const auto size = static_cast<std::size_t>(fileBytes);
  std::vector<std::uint8_t> encodedBytes(size);
  std::ifstream stream(path, std::ios::binary);
  stream.read(reinterpret_cast<char *>(encodedBytes.data()),
              static_cast<std::streamsize>(encodedBytes.size()));
  if (!stream || static_cast<std::size_t>(stream.gcount()) != fileBytes)
    return failure("Could not read the complete audio file", path);
  return decode(encodedBytes, path);
}

std::string supportedAudioFileWildcard() {
  juce::AudioFormatManager formats;
  registerAudioFormats(formats);
  auto wildcard = formats.getWildcardForAllFormats().toStdString();
  if (!wildcard.empty())
    wildcard += ';';
  wildcard += "*.ondabuffer";
  return wildcard;
}

} // namespace onda::plugin
