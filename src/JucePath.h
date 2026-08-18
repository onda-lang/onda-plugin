#pragma once

#include <juce_core/juce_core.h>

#include <cstring>
#include <filesystem>
#include <string>

namespace onda::plugin {

inline std::filesystem::path pathFromJuce(const juce::String &text) {
  const auto *bytes = text.toRawUTF8();
  const auto size = std::strlen(bytes);
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t *>(bytes),
                    reinterpret_cast<const char8_t *>(bytes) + size));
}

inline juce::String pathToJuce(const std::filesystem::path &path) {
  const auto bytes = path.u8string();
  return juce::String::fromUTF8(reinterpret_cast<const char *>(bytes.data()),
                                static_cast<int>(bytes.size()));
}

} // namespace onda::plugin
