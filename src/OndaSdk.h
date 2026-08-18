#pragma once

#include <onda.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace onda::plugin {

struct ProgramDeleter {
  void operator()(onda_program_t *value) const noexcept;
};
struct InstanceDeleter {
  void operator()(onda_instance_t *value) const noexcept;
};
struct ManifestDeleter {
  void operator()(onda_source_manifest_t *value) const noexcept;
};
struct ProjectImageDeleter {
  void operator()(onda_project_image_t *value) const noexcept;
};
struct MaterializationDeleter {
  void operator()(onda_project_materialization_plan_t *value) const noexcept;
};

using ProgramHandle = std::unique_ptr<onda_program_t, ProgramDeleter>;
using InstanceHandle = std::unique_ptr<onda_instance_t, InstanceDeleter>;
using ManifestHandle = std::unique_ptr<onda_source_manifest_t, ManifestDeleter>;
using ProjectImageHandle =
    std::unique_ptr<onda_project_image_t, ProjectImageDeleter>;
using MaterializationHandle =
    std::unique_ptr<onda_project_materialization_plan_t,
                    MaterializationDeleter>;

struct Diagnostic {
  int code{};
  int line{};
  int column{};
  int endLine{};
  int endColumn{};
  std::string message;
  std::string file;
  std::string trace;

  [[nodiscard]] bool empty() const noexcept { return message.empty(); }
};

class OndaDiagnostic final {
public:
  OndaDiagnostic() = default;
  ~OndaDiagnostic();

  OndaDiagnostic(const OndaDiagnostic &) = delete;
  OndaDiagnostic &operator=(const OndaDiagnostic &) = delete;

  [[nodiscard]] onda_diag_t *outParameter() noexcept;
  [[nodiscard]] Diagnostic copy() const;

private:
  onda_diag_t value_{};
};

struct ProjectImage {
  std::shared_ptr<const std::vector<std::uint8_t>> bytes;

  [[nodiscard]] bool valid() const noexcept {
    return bytes != nullptr && !bytes->empty();
  }

  friend bool operator==(const ProjectImage &, const ProjectImage &) = default;
};

struct ProjectBufferAsset {
  std::string name;
  std::vector<std::uint8_t> encodedBytes;
};

struct ProjectBufferInfo {
  std::string name;
  int elementType{-1};
  std::int64_t frames{};
  std::int64_t channels{};
  float sampleRate{};
};

struct MaterializedProjectFile {
  std::filesystem::path relativePath;
  std::vector<std::uint8_t> bytes;
};

struct CompileResult {
  ProgramHandle program;
  ManifestHandle manifest;
  std::vector<std::filesystem::path> watchPaths;
  ProjectImage projectImage;
  std::vector<ProjectBufferInfo> projectBuffers;
  Diagnostic diagnostic;
};

[[nodiscard]] CompileResult compileFile(const std::filesystem::path &path,
                                        double sampleRate, int blockSize);
[[nodiscard]] CompileResult compileProjectImage(const ProjectImage &image,
                                                double sampleRate,
                                                int blockSize);
[[nodiscard]] ProjectImage captureProjectImage(
    const std::filesystem::path &entry, const onda_source_manifest_t *manifest,
    std::span<const ProjectBufferAsset> buffers, Diagnostic &diagnostic);
[[nodiscard]] bool validateProjectImage(const ProjectImage &image,
                                        Diagnostic &diagnostic);
using MaterializedProjectVisitor =
    std::function<bool(MaterializedProjectFile &&)>;
[[nodiscard]] bool
visitMaterializedProjectFiles(const ProjectImage &image, Diagnostic &diagnostic,
                              const MaterializedProjectVisitor &visitor);
[[nodiscard]] std::vector<std::uint8_t>
encodeF32BufferAsset(std::span<const float> samples, int frames, int channels,
                     float sampleRate, Diagnostic &diagnostic);

} // namespace onda::plugin
