#pragma once

#include <onda.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace onda::plugin {

// Typed vectors provide the alignment and lifetime required by
// onda_bind_buffer.
struct BufferSamples {
  using Storage =
      std::variant<std::vector<std::uint8_t>, std::vector<std::int32_t>,
                   std::vector<std::int64_t>, std::vector<float>,
                   std::vector<double>>;
  Storage values{std::vector<float>{}};

  [[nodiscard]] int elementType() const noexcept {
    constexpr int types[]{ONDA_PRIMITIVE_BOOL, ONDA_PRIMITIVE_I32,
                          ONDA_PRIMITIVE_I64, ONDA_PRIMITIVE_F32,
                          ONDA_PRIMITIVE_F64};
    return types[values.index()];
  }
  [[nodiscard]] void *data() noexcept {
    return std::visit([](auto &samples) -> void * { return samples.data(); },
                      values);
  }
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return std::visit(
        [](const auto &samples) { return std::as_bytes(std::span{samples}); },
        values);
  }
  [[nodiscard]] bool empty() const noexcept { return bytes().empty(); }
};

struct DecodedAudioFile {
  BufferSamples samples;
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
// Lowercase filename suffixes, including the leading dot.
[[nodiscard]] const std::vector<std::string> &supportedAudioFileExtensions();
[[nodiscard]] std::string supportedAudioFileWildcard();

} // namespace onda::plugin
