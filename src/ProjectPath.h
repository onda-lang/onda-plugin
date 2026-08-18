#pragma once

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
  return std::string_view(reinterpret_cast<const char *>(extension.data()),
                          extension.size()) == ".ondaproject";
}

} // namespace onda::plugin
