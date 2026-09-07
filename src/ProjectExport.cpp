#include "ProjectExport.h"

#include "ProjectPath.h"

#include <chrono>
#include <fstream>
#include <limits>
#include <system_error>

namespace onda::plugin {
namespace {

ProjectExportResult failure(std::string message) {
  return {.projectFile = {}, .error = std::move(message)};
}

class StagingDirectory final {
public:
  explicit StagingDirectory(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~StagingDirectory() {
    if (!committed_) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }
  void commit() noexcept { committed_ = true; }

private:
  std::filesystem::path path_;
  bool committed_{};
};

} // namespace

ProjectExportResult
exportProject(const ProjectImage &projectImage,
              const std::filesystem::path &destinationDirectory) {
  std::error_code filesystemError;
  auto destination =
      std::filesystem::absolute(destinationDirectory, filesystemError);
  if (filesystemError || destination.filename().empty() ||
      destination == destination.root_path()) {
    return failure("The project destination is invalid");
  }
  destination = destination.lexically_normal();
  const auto parent = destination.parent_path();
  if (!std::filesystem::is_directory(parent, filesystemError) ||
      filesystemError) {
    return failure("The project destination parent does not exist");
  }

  const auto destinationExists =
      std::filesystem::exists(destination, filesystemError);
  if (filesystemError)
    return failure("Could not inspect the project destination");
  if (destinationExists &&
      (std::filesystem::is_symlink(
           std::filesystem::symlink_status(destination, filesystemError)) ||
       filesystemError ||
       !std::filesystem::is_directory(destination, filesystemError) ||
       filesystemError ||
       !std::filesystem::is_empty(destination, filesystemError) ||
       filesystemError)) {
    return failure("Choose a new or empty project directory");
  }

  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path staging;
  for (int attempt = 0; attempt < 100; ++attempt) {
    auto name = destination.filename();
    name += ".onda-staging-" + std::to_string(stamp) + '-' +
            std::to_string(attempt);
    staging = parent / name;
    if (std::filesystem::create_directory(staging, filesystemError))
      break;
    if (filesystemError && filesystemError != std::errc::file_exists)
      return failure("Could not create the project staging directory");
    filesystemError.clear();
    staging.clear();
  }
  if (staging.empty())
    return failure("Could not reserve a project staging directory");
  StagingDirectory cleanup(staging);

  ProjectExportResult result;
  std::string exportError;
  Diagnostic diagnostic;
  const auto materialized = visitMaterializedProjectFiles(
      projectImage, diagnostic, [&](MaterializedProjectFile &&file) {
        if (!isSafeRelativeProjectPath(file.relativePath)) {
          exportError = "Onda returned an unsafe project path";
          return false;
        }
        if (isOndaProjectPath(file.relativePath)) {
          if (!result.projectFile.empty()) {
            exportError = "Onda returned more than one project manifest";
            return false;
          }
          result.projectFile =
              destination / file.relativePath.lexically_normal();
        }

        const auto output = staging / file.relativePath.lexically_normal();
        if (!std::filesystem::create_directories(output.parent_path(),
                                                 filesystemError) &&
            filesystemError) {
          exportError = "Could not create an exported project directory";
          return false;
        }
        if (file.bytes.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::streamsize>::max())) {
          exportError = "A materialized project file is too large";
          return false;
        }
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(file.bytes.data()),
                     static_cast<std::streamsize>(file.bytes.size()));
        stream.close();
        if (!stream) {
          exportError = "Could not write an exported project file";
          return false;
        }
        return true;
      });
  if (!materialized) {
    return failure(exportError.empty()
                       ? (diagnostic.message.empty()
                              ? "Onda returned an empty project export"
                              : std::move(diagnostic.message))
                       : std::move(exportError));
  }
  if (result.projectFile.empty())
    return failure("Onda returned no project manifest");
  if (!std::filesystem::is_regular_file(
          staging / result.projectFile.lexically_relative(destination),
          filesystemError) ||
      filesystemError)
    return failure("Onda returned no usable project manifest");

  if (destinationExists &&
      (!std::filesystem::remove(destination, filesystemError) ||
       filesystemError)) {
    return failure("Could not replace the empty project destination");
  }
  std::filesystem::rename(staging, destination, filesystemError);
  if (filesystemError) {
    if (destinationExists) {
      std::error_code ignored;
      std::filesystem::create_directory(destination, ignored);
    }
    return failure("Could not publish the exported project directory");
  }
  cleanup.commit();
  return result;
}

} // namespace onda::plugin
