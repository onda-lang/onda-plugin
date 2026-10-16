#include "OndaSdk.h"

#include "ProjectPath.h"

#include <algorithm>
#include <charconv>
#include <juce_core/juce_core.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

namespace onda::plugin {
namespace {

std::string copyString(const char *text) {
  return text == nullptr ? std::string{} : std::string{text};
}

std::string pathString(const std::filesystem::path &path) {
  // Project-image file names require portable '/' separators on every OS.
  const auto bytes = path.generic_u8string();
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
                                      const int blockSize,
                                      const bool fastMath) {
  return {
      .fast_math = fastMath ? 1 : 0,
      .sample_rate = static_cast<float>(sampleRate),
      .block_size = blockSize,
      .const_inputs = nullptr,
      .const_input_count = 0U,
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

std::vector<onda_project_buffer_asset_t>
rawBufferAssets(const std::span<const ProjectBufferAsset> buffers) {
  std::vector<onda_project_buffer_asset_t> result;
  result.reserve(buffers.size());
  for (const auto &buffer : buffers) {
    result.push_back({
        .name_utf8 = buffer.name.c_str(),
        .ondabuffer_bytes = buffer.encodedBytes.data(),
        .ondabuffer_byte_count = buffer.encodedBytes.size(),
    });
  }
  return result;
}

// Locate the SDK-generated root buffers object without decoding other fields.
// JUCE's JSON number parser truncates small decimal constants; those bytes must
// remain opaque when changing buffer bindings. Strings may contain any braces.
std::optional<std::pair<std::size_t, std::size_t>>
bufferBindingsRange(const std::string_view json) {
  int depth{};
  bool quoted{}, escaped{};
  std::size_t stringStart{};
  std::optional<std::size_t> objectStart;
  for (std::size_t index = 0; index < json.size(); ++index) {
    const auto character = json[index];
    if (quoted) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        quoted = false;
        if (depth == 1 && json.substr(stringStart, index - stringStart + 1) ==
                              "\"buffers\"") {
          auto next = json.find_first_not_of(" \t\r\n", index + 1);
          if (next != std::string_view::npos && json[next] == ':') {
            next = json.find_first_not_of(" \t\r\n", next + 1);
            if (next != std::string_view::npos && json[next] == '{')
              objectStart = next;
          }
        }
      }
    } else if (character == '"') {
      quoted = true;
      stringStart = index;
    } else if (character == '{' || character == '[') {
      ++depth;
    } else if (character == '}' || character == ']') {
      --depth;
      if (objectStart && index >= *objectStart && depth == 1)
        return std::pair{*objectStart, index + 1};
    }
  }
  return std::nullopt;
}

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
  auto root = canonicalProject.parent_path();
  std::vector<std::filesystem::path> canonicalPaths;
  canonicalPaths.reserve(watchPaths.size());
  for (const auto &watchPath : watchPaths) {
    const auto canonicalPath = std::filesystem::canonical(watchPath, errorCode);
    if (errorCode) {
      diagnostic.message =
          "Could not resolve an Onda project file for checkpointing";
      return {};
    }
    // The transport workspace must also contain imports outside the manifest
    // directory. Onda still owns manifest containment and source resolution.
    while (!isSafeRelativeProjectPath(canonicalPath.lexically_relative(root))) {
      const auto parent = root.parent_path();
      if (parent == root) {
        diagnostic.message =
            "Onda project files do not share a checkpoint root";
        return {};
      }
      root = parent;
    }
    canonicalPaths.push_back(canonicalPath);
  }

  std::vector<StoredProjectFile> storage;
  storage.reserve(canonicalPaths.size());
  for (const auto &canonicalPath : canonicalPaths) {
    const auto relativePath = canonicalPath.lexically_relative(root);
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
                          const double sampleRate, const int blockSize,
                          const bool fastMath) {
  if (!validCompileConfiguration(sampleRate, blockSize)) {
    CompileResult result;
    result.diagnostic.message = "Invalid host sample rate or block size";
    return result;
  }

  const auto nativePath = pathString(path);
  const auto options = compileOptions(sampleRate, blockSize, fastMath);
  onda_source_manifest_t *rawManifest{};
  OndaDiagnostic rawDiagnostic;
  ProgramHandle program{onda_compile_file(nativePath.c_str(), &options,
                                          &rawManifest,
                                          rawDiagnostic.outParameter()),
                        ProgramDeleter{}};
  ManifestHandle manifest{rawManifest};
  CompileResult result{
      .sampleRate = sampleRate,
      .blockSize = blockSize,
      .fastMath = fastMath,
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
  if (result.program && !result.projectImage.valid()) {
    result.projectImage =
        captureProjectImage(path, result.manifest.get(), {}, result.diagnostic);
    if (!result.projectImage.valid())
      result.program.reset();
  }
  return result;
}

CompileResult compileProjectImage(const ProjectImage &image,
                                  const double sampleRate, const int blockSize,
                                  const bool fastMath) {
  CompileResult result;
  result.sampleRate = sampleRate;
  result.blockSize = blockSize;
  result.fastMath = fastMath;
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

  const auto options = compileOptions(sampleRate, blockSize, fastMath);
  OndaDiagnostic rawDiagnostic;
  result.program =
      ProgramHandle{onda_project_image_compile(handle.get(), &options,
                                               rawDiagnostic.outParameter()),
                    ProgramDeleter{}};
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

  const auto rawBuffers = rawBufferAssets(buffers);

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

ProjectImage
withProjectBufferOverrides(const ProjectImage &image,
                           const std::span<const ProjectBufferAsset> buffers,
                           Diagnostic &diagnostic) {
  auto handle = deserialize(image, diagnostic);
  if (!handle)
    return {};
  const auto rawBuffers = rawBufferAssets(buffers);
  OndaDiagnostic rawDiagnostic;
  ProjectImageHandle updated{onda_project_image_with_buffer_overrides(
      handle.get(), rawBuffers.data(), rawBuffers.size(),
      rawDiagnostic.outParameter())};
  diagnostic = rawDiagnostic.copy();
  if (!updated) {
    if (diagnostic.empty())
      diagnostic.message = "Onda could not update the project buffer assets";
    return {};
  }
  return serialize(updated.get(), diagnostic);
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

ProjectImage withoutProjectBuffers(const ProjectImage &image,
                                   const std::span<const std::string> names,
                                   Diagnostic &diagnostic) {
  if (names.empty())
    return image;
  // Edit the SDK's public materialization manifest, then let the SDK rebuild
  // and validate the image. The original image and its defaults stay immutable.
  std::vector<StoredProjectFile> files;
  if (!visitMaterializedProjectFiles(
          image, diagnostic, [&](MaterializedProjectFile &&file) {
            files.push_back(
                {pathString(file.relativePath), std::move(file.bytes)});
            return true;
          }))
    return {};
  const auto manifest =
      std::ranges::find(files, "project.ondaproject", &StoredProjectFile::path);
  if (manifest == files.end()) {
    diagnostic.message = "Onda export omitted the project manifest";
    return {};
  }
  const std::string original(manifest->bytes.begin(), manifest->bytes.end());
  const auto range = bufferBindingsRange(original);
  if (!range)
    return image; // The image already has no asset bindings.
  auto bindings = juce::JSON::parse(
      juce::String::fromUTF8(original.data() + range->first,
                             static_cast<int>(range->second - range->first)));
  if (auto *object = bindings.getDynamicObject()) {
    for (const auto &name : names) {
      const auto bracket = name.find('[');
      if (bracket == std::string::npos) {
        object->removeProperty(
            juce::Identifier{juce::String::fromUTF8(name.c_str())});
      } else if (name.back() == ']') {
        int index{};
        const auto end = name.data() + name.size() - 1;
        const auto parsed =
            std::from_chars(name.data() + bracket + 1, end, index);
        auto elements = object->getProperty(juce::Identifier{
            juce::String::fromUTF8(name.substr(0, bracket).c_str())});
        if (parsed.ec == std::errc{} && parsed.ptr == end && index >= 0 &&
            elements.isArray() && index < elements.size())
          elements.getArray()->set(index, juce::var{});
      }
    }
  }
  auto updatedManifest = original;
  updatedManifest.replace(range->first, range->second - range->first,
                          juce::JSON::toString(bindings).toStdString());
  manifest->bytes.assign(updatedManifest.begin(), updatedManifest.end());
  std::vector<onda_project_file_t> raw;
  raw.reserve(files.size());
  for (const auto &file : files)
    raw.push_back({file.path.c_str(), file.bytes.data(), file.bytes.size()});
  OndaDiagnostic rawDiagnostic;
  ProjectImageHandle updated{onda_project_image_load_files(
      raw.data(), raw.size(), "project.ondaproject",
      rawDiagnostic.outParameter())};
  diagnostic = rawDiagnostic.copy();
  if (!updated) {
    if (diagnostic.empty())
      diagnostic.message = "Onda could not clear the project buffer bindings";
    return {};
  }
  return serialize(updated.get(), diagnostic);
}

std::vector<std::uint8_t>
encodeBufferAsset(const int elementType,
                  const std::span<const std::byte> samples, const int frames,
                  const int channels, const float sampleRate,
                  Diagnostic &diagnostic) {
  if (frames <= 0 || channels <= 0 || !std::isfinite(sampleRate) ||
      sampleRate <= 0.0F) {
    diagnostic.message = "Cannot encode invalid audio buffer dimensions";
    return {};
  }
  OndaDiagnostic rawDiagnostic;
  const auto sampleBytes = samples.size_bytes();
  const auto required = onda_buffer_asset_encode(
      elementType, static_cast<std::uint32_t>(frames),
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
      elementType, static_cast<std::uint32_t>(frames),
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

std::vector<std::uint8_t> encodeF32BufferAsset(std::span<const float> samples,
                                               int frames, int channels,
                                               float sampleRate,
                                               Diagnostic &diagnostic) {
  return encodeBufferAsset(ONDA_PRIMITIVE_F32, std::as_bytes(samples), frames,
                           channels, sampleRate, diagnostic);
}

} // namespace onda::plugin
