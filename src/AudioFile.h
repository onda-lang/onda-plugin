#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace onda::plugin {

struct DecodedAudioFile {
  std::vector<float> interleavedSamples;
  int frames{};
  int channels{};
  float sampleRate{};
};

struct AudioFileResult {
  std::optional<DecodedAudioFile> audio;
  std::string error;
};

[[nodiscard]] AudioFileResult
decodeAudioFile(const std::filesystem::path &path);
[[nodiscard]] std::string supportedAudioFileWildcard();

} // namespace onda::plugin
