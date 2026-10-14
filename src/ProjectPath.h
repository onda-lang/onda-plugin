#pragma once

#include <algorithm>
#include <filesystem>
#include <string_view>

namespace onda::plugin {

inline bool isSafeRelativeProjectPath(const std::filesystem::path &path) {
  if (path.empty() || path.is_absolute() || path.has_root_path())
    return false;
  const auto normalized = path.lexically_normal();
  return !normalized.empty() && normalized != "." &&
         *normalized.begin() != "..";
}

inline bool isOndaProjectPath(const std::filesystem::path &path) {
  const auto extension = path.extension().u8string();
  return std::ranges::equal(extension, std::u8string_view{u8".ondaproject"},
                            [](const char8_t actual, const char8_t expected) {
                              return (actual >= u8'A' && actual <= u8'Z'
                                          ? actual + (u8'a' - u8'A')
                                          : actual) == expected;
                            });
}

} // namespace onda::plugin
