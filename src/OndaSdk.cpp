#include "OndaSdk.h"

#include "ProjectPath.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>

namespace onda::plugin {
namespace {

std::string copyString(const char *text) {
  return text == nullptr ? std::string{} : std::string{text};
}

std::string pathString(const std::filesystem::path &path) {
  const auto bytes = path.u8string();
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

std::filesystem::path pathFromString(const char *path) {
  if (path == nullptr)
    return {};
  const auto length = std::strlen(path);
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t *>(path),
                    reinterpret_cast<const char8_t *>(path) + length));
}

std::vector<std::filesystem::path>
copyWatchPaths(const onda_source_manifest_t *manifest) {
  std::vector<std::filesystem::path> paths;
  if (manifest == nullptr)
    return paths;

  const auto count = onda_source_manifest_watch_count(manifest);
  if (count <= 0)
    return paths;

  paths.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    const auto *path = onda_source_manifest_watch_path(manifest, index);
    if (path != nullptr)
      paths.push_back(pathFromString(path));
  }
  return paths;
}

onda_compile_options_t compileOptions(const double sampleRate,
                                      const int blockSize) {
  return {
      .fast_math = 0,
      .sample_rate = static_cast<float>(sampleRate),
      .block_size = blockSize,
  };
}

bool validCompileConfiguration(const double sampleRate, const int blockSize) {
  return std::isfinite(sampleRate) && sampleRate > 0.0 && blockSize > 0 &&
         sampleRate <= static_cast<double>(std::numeric_limits<float>::max());
}

ProjectImageHandle deserialize(const ProjectImage &image,
                               Diagnostic &diagnostic) {
  if (!image.valid()) {
    diagnostic.message = "Saved Onda project image is empty";
    return {};
  }
  OndaDiagnostic rawDiagnostic;
  ProjectImageHandle handle{onda_project_image_deserialize(
      image.bytes->data(), image.bytes->size(), rawDiagnostic.outParameter())};
  diagnostic = rawDiagnostic.copy();
  if (!handle && diagnostic.empty())
    diagnostic.message = "Saved Onda project image is invalid";
  return handle;
}

ProjectImage serialize(const onda_project_image_t *image,
                       Diagnostic &diagnostic) {
  OndaDiagnostic rawDiagnostic;
  const auto required = onda_project_image_serialize(
      image, nullptr, 0, rawDiagnostic.outParameter());
  diagnostic = rawDiagnostic.copy();
  if (required <= 0 || static_cast<std::uint64_t>(required) >
                           std::vector<std::uint8_t>{}.max_size()) {
    if (diagnostic.empty())
      diagnostic.message = "Onda could not serialize the project image";
    return {};
  }

  auto bytes = std::make_shared<std::vector<std::uint8_t>>(
      static_cast<std::size_t>(required));
  const auto written = onda_project_image_serialize(
      image, bytes->data(), bytes->size(), rawDiagnostic.outParameter());
  diagnostic = rawDiagnostic.copy();
  if (written != required) {
    if (diagnostic.empty())
      diagnostic.message = "Onda could not serialize the project image";
    return {};
  }
  diagnostic = {};
  return {.bytes = std::move(bytes)};
}

std::vector<ProjectBufferInfo>
copyProjectBuffers(const onda_project_image_t *image, Diagnostic &diagnostic) {
  const auto count = onda_project_image_buffer_count(image);
  if (count < 0) {
    diagnostic.message = "Onda returned invalid project buffer metadata";
    return {};
  }
  std::vector<ProjectBufferInfo> buffers;
  buffers.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    const auto *name = onda_project_image_buffer_name(image, index);
    ProjectBufferInfo info{
        .name = copyString(name),
        .elementType = onda_project_image_buffer_element_type(image, index),
        .frames = onda_project_image_buffer_frames(image, index),
        .channels = onda_project_image_buffer_channels(image, index),
        .sampleRate = onda_project_image_buffer_sample_rate(image, index),
    };
    if (info.name.empty() || info.elementType < 0 || info.frames <= 0 ||
        info.channels <= 0 || !std::isfinite(info.sampleRate) ||
        info.sampleRate <= 0.0F) {
      diagnostic.message = "Onda returned invalid project buffer metadata";
      return {};
    }
    buffers.push_back(std::move(info));
  }
  return buffers;
}

struct StoredProjectFile {
  std::string path;
  std::vector<std::uint8_t> bytes;
};

ProjectImage loadFilesystemProjectImage(
    const std::filesystem::path &projectPath,
    const std::span<const std::filesystem::path> watchPaths,
    std::vector<ProjectBufferInfo> &buffers, Diagnostic &diagnostic) {
  std::error_code errorCode;
  const auto canonicalProject =
      std::filesystem::canonical(projectPath, errorCode);
  if (errorCode) {
    diagnostic.message = "Could not resolve the Onda project for checkpointing";
    return {};
  }
  const auto root = canonicalProject.parent_path();

  std::vector<StoredProjectFile> storage;
  storage.reserve(watchPaths.size());
  for (const auto &watchPath : watchPaths) {
    const auto canonicalPath = std::filesystem::canonical(watchPath, errorCode);
    if (errorCode) {
      diagnostic.message =
          "Could not resolve an Onda project file for checkpointing";
      return {};
    }
    const auto relativePath = canonicalPath.lexically_relative(root);
    if (!isSafeRelativeProjectPath(relativePath)) {
      diagnostic.message =
          "An Onda project file is outside the checkpoint root";
      return {};
    }

    const auto byteCount = std::filesystem::file_size(canonicalPath, errorCode);
    if (errorCode || byteCount > std::vector<std::uint8_t>{}.max_size() ||
        byteCount > static_cast<std::uintmax_t>(
                        std::numeric_limits<std::streamsize>::max())) {
      diagnostic.message = "An Onda project file is too large to checkpoint";
      return {};
    }

    StoredProjectFile file{.path = pathString(relativePath),
                           .bytes = std::vector<std::uint8_t>(
                               static_cast<std::size_t>(byteCount))};
    std::ifstream stream(canonicalPath, std::ios::binary);
    if (!stream ||
        (!file.bytes.empty() &&
         !stream.read(reinterpret_cast<char *>(file.bytes.data()),
                      static_cast<std::streamsize>(file.bytes.size())))) {
      diagnostic.message =
          "Could not read an Onda project file for checkpointing";
      return {};
    }
    storage.push_back(std::move(file));
  }

  std::vector<onda_project_file_t> files;
  files.reserve(storage.size());
  for (const auto &file : storage) {
    files.push_back({.path_utf8 = file.path.c_str(),
                     .bytes = file.bytes.data(),
                     .byte_count = file.bytes.size()});
  }

  const auto selectedProject =
      pathString(canonicalProject.lexically_relative(root));
  OndaDiagnostic rawDiagnostic;
  ProjectImageHandle image{onda_project_image_load_files(
      files.empty() ? nullptr : files.data(), files.size(),
      selectedProject.c_str(), rawDiagnostic.outParameter())};
  diagnostic = rawDiagnostic.copy();
  if (!image) {
    if (diagnostic.empty())
      diagnostic.message = "Onda could not capture the filesystem project";
    return {};
  }

  buffers = copyProjectBuffers(image.get(), diagnostic);
  if (!diagnostic.empty())
    return {};
  return serialize(image.get(), diagnostic);
}

} // namespace

void ProgramDeleter::operator()(onda_program_t *value) const noexcept {
  onda_program_destroy(value);
}

void InstanceDeleter::operator()(onda_instance_t *value) const noexcept {
  onda_instance_destroy(value);
}

void ManifestDeleter::operator()(onda_source_manifest_t *value) const noexcept {
  onda_source_manifest_destroy(value);
}

void ProjectImageDeleter::operator()(
    onda_project_image_t *value) const noexcept {
  onda_project_image_destroy(value);
}

void MaterializationDeleter::operator()(
    onda_project_materialization_plan_t *value) const noexcept {
  onda_project_materialization_destroy(value);
}

OndaDiagnostic::~OndaDiagnostic() { onda_diag_dispose(&value_); }

onda_diag_t *OndaDiagnostic::outParameter() noexcept {
  onda_diag_dispose(&value_);
  return &value_;
}

Diagnostic OndaDiagnostic::copy() const {
  return {
      .code = value_.code,
      .line = value_.line,
      .column = value_.column,
      .endLine = value_.end_line,
      .endColumn = value_.end_column,
      .message = copyString(value_.message),
      .file = copyString(value_.file),
      .trace = copyString(value_.trace),
  };
}

CompileResult compileFile(const std::filesystem::path &path,
                          const double sampleRate, const int blockSize) {
  if (!validCompileConfiguration(sampleRate, blockSize)) {
    CompileResult result;
    result.diagnostic.message = "Invalid host sample rate or block size";
    return result;
  }

  const auto nativePath = pathString(path);
  const auto options = compileOptions(sampleRate, blockSize);
  onda_source_manifest_t *rawManifest{};
  OndaDiagnostic rawDiagnostic;
  ProgramHandle program{onda_compile_file(nativePath.c_str(), &options,
                                          &rawManifest,
                                          rawDiagnostic.outParameter())};
  ManifestHandle manifest{rawManifest};
  CompileResult result{
      .program = std::move(program),
      .manifest = std::move(manifest),
      .watchPaths = copyWatchPaths(rawManifest),
      .projectImage = {},
      .projectBuffers = {},
      .diagnostic = rawDiagnostic.copy(),
  };
  if (result.program && isOndaProjectPath(path)) {
    result.projectImage = loadFilesystemProjectImage(
        path, result.watchPaths, result.projectBuffers, result.diagnostic);
    if (!result.projectImage.valid())
      result.program.reset();
  }
  return result;
}

CompileResult compileProjectImage(const ProjectImage &image,
                                  const double sampleRate,
                                  const int blockSize) {
  CompileResult result;
  result.projectImage = image;
  if (!validCompileConfiguration(sampleRate, blockSize)) {
    result.diagnostic.message = "Invalid host sample rate or block size";
    return result;
  }

  auto handle = deserialize(image, result.diagnostic);
  if (!handle)
    return result;
  result.projectBuffers = copyProjectBuffers(handle.get(), result.diagnostic);
  if (!result.diagnostic.empty())
    return result;

  const auto options = compileOptions(sampleRate, blockSize);
  OndaDiagnostic rawDiagnostic;
  result.program.reset(onda_project_image_compile(
      handle.get(), &options, rawDiagnostic.outParameter()));
  result.diagnostic = rawDiagnostic.copy();
  return result;
}

ProjectImage captureProjectImage(
    const std::filesystem::path &entry, const onda_source_manifest_t *manifest,
    const std::span<const ProjectBufferAsset> buffers, Diagnostic &diagnostic) {
  if (manifest == nullptr) {
    diagnostic.message = "Onda compilation returned no source manifest";
    return {};
  }

  std::vector<onda_project_buffer_asset_t> rawBuffers;
  rawBuffers.reserve(buffers.size());
  for (const auto &buffer : buffers) {
    if (buffer.name.empty() || buffer.encodedBytes.empty()) {
      diagnostic.message = "Cannot capture an invalid project buffer asset";
      return {};
    }
    rawBuffers.push_back({
        .name_utf8 = buffer.name.c_str(),
        .ondabuffer_bytes = buffer.encodedBytes.data(),
        .ondabuffer_byte_count = buffer.encodedBytes.size(),
    });
  }

  const auto entryString = pathString(entry);
  const auto sourceRoot = pathString(entry.parent_path());
  OndaDiagnostic rawDiagnostic;
  ProjectImageHandle image{onda_project_image_capture(
      entryString.c_str(), sourceRoot.c_str(), manifest,
      rawBuffers.empty() ? nullptr : rawBuffers.data(), rawBuffers.size(),
      rawDiagnostic.outParameter())};
  diagnostic = rawDiagnostic.copy();
  if (!image) {
    if (diagnostic.empty())
      diagnostic.message = "Onda could not capture the project image";
    return {};
  }
  return serialize(image.get(), diagnostic);
}

bool validateProjectImage(const ProjectImage &image, Diagnostic &diagnostic) {
  return deserialize(image, diagnostic) != nullptr;
}

bool visitMaterializedProjectFiles(const ProjectImage &image,
                                   Diagnostic &diagnostic,
                                   const MaterializedProjectVisitor &visitor) {
  auto handle = deserialize(image, diagnostic);
  if (!handle)
    return false;

  OndaDiagnostic rawDiagnostic;
  MaterializationHandle plan{onda_project_image_materialize(
      handle.get(), rawDiagnostic.outParameter())};
  diagnostic = rawDiagnostic.copy();
  if (!plan) {
    if (diagnostic.empty())
      diagnostic.message = "Onda could not materialize the project image";
    return false;
  }

  const auto count = onda_project_materialization_file_count(plan.get());
  if (count <= 0) {
    diagnostic.message = "Onda returned an empty project materialization";
    return false;
  }
  for (int index = 0; index < count; ++index) {
    const auto *path =
        onda_project_materialization_file_path(plan.get(), index);
    const auto required =
        onda_project_materialization_file_bytes(plan.get(), index, nullptr, 0);
    if (path == nullptr || required < 0 ||
        static_cast<std::uint64_t>(required) >
            std::vector<std::uint8_t>{}.max_size()) {
      diagnostic.message = "Onda returned an invalid materialized project file";
      return false;
    }
    MaterializedProjectFile file{
        .relativePath = pathFromString(path),
        .bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(required)),
    };
    const auto written = onda_project_materialization_file_bytes(
        plan.get(), index, file.bytes.data(), file.bytes.size());
    if (written != required) {
      diagnostic.message = "Onda could not copy a materialized project file";
      return false;
    }
    if (!visitor(std::move(file))) {
      if (diagnostic.empty())
        diagnostic.message = "The materialized Onda project was rejected";
      return false;
    }
  }
  diagnostic = {};
  return true;
}

std::vector<std::uint8_t>
encodeF32BufferAsset(const std::span<const float> samples, const int frames,
                     const int channels, const float sampleRate,
                     Diagnostic &diagnostic) {
  if (frames <= 0 || channels <= 0 || !std::isfinite(sampleRate) ||
      sampleRate <= 0.0F ||
      samples.size() != static_cast<std::size_t>(frames) *
                            static_cast<std::size_t>(channels)) {
    diagnostic.message = "Cannot encode invalid audio buffer dimensions";
    return {};
  }
  OndaDiagnostic rawDiagnostic;
  const auto sampleBytes = samples.size_bytes();
  const auto required = onda_buffer_asset_encode(
      ONDA_PRIMITIVE_F32, static_cast<std::uint32_t>(frames),
      static_cast<std::uint32_t>(channels), sampleRate, samples.data(),
      sampleBytes, nullptr, 0, rawDiagnostic.outParameter());
  diagnostic = rawDiagnostic.copy();
  if (required <= 0 || static_cast<std::uint64_t>(required) >
                           std::vector<std::uint8_t>{}.max_size()) {
    if (diagnostic.empty())
      diagnostic.message = "Onda could not encode the project buffer asset";
    return {};
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(required));
  const auto written = onda_buffer_asset_encode(
      ONDA_PRIMITIVE_F32, static_cast<std::uint32_t>(frames),
      static_cast<std::uint32_t>(channels), sampleRate, samples.data(),
      sampleBytes, bytes.data(), bytes.size(), rawDiagnostic.outParameter());
  diagnostic = rawDiagnostic.copy();
  if (written != required) {
    if (diagnostic.empty())
      diagnostic.message = "Onda could not encode the project buffer asset";
    return {};
  }
  diagnostic = {};
  return bytes;
}

} // namespace onda::plugin
