#pragma once

#include "Engine.h"

#include <filesystem>
#include <string>

namespace onda::plugin {

struct ProjectExportResult {
  std::filesystem::path projectFile;
  std::string error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error.empty() && !projectFile.empty();
  }
};

[[nodiscard]] ProjectExportResult
exportProject(const ProjectImage &projectImage,
              const std::filesystem::path &destinationDirectory);

} // namespace onda::plugin
