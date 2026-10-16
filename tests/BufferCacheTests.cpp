#include "Engine.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"
#include "TestRunner.h"
#include "Worker.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace onda::plugin {

// This executable substitutes only the native notification backend. File IO,
// hashing, preparation and DSP stay real; edits precede notification delivery.
struct FilesystemWatcher::SharedState {};
class FilesystemWatcher::Impl {};
FilesystemWatcher::FilesystemWatcher(Callback) {}
FilesystemWatcher::~FilesystemWatcher() = default;
FilesystemWatcher::Paths
FilesystemWatcher::watch(std::span<const std::filesystem::path>) {
  return {};
}
std::uint64_t FilesystemWatcher::revision() const noexcept { return 0; }

} // namespace onda::plugin

namespace {
using namespace onda::plugin;

bool writeText(const std::filesystem::path &path, std::string_view text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
  return file.good();
}

bool renders(PreparedEngine &engine, float expected) {
  ParameterState parameters;
  std::array<float, 8> left{}, right{};
  const std::array outputs{left.data(), right.data()};
  if (!engine.process(nullptr, outputs.data(), 8, {}, parameters) ||
      !test::withinTolerance(left[0] - expected, 1.0e-6F) ||
      !test::withinTolerance(right[0] - expected, 1.0e-6F)) {
    std::cerr << "cache rendered " << left[0] << ", " << right[0]
              << "; expected " << expected << '\n';
    return false;
  }
  return true;
}

struct Fixture {
  const std::filesystem::path root;
  const std::filesystem::path source;
  const std::filesystem::path asset;
  SpscSlot<PreparedEngine *> replacements, retirements;
  std::atomic<bool> deactivate{};
  Worker worker{Product::effect, replacements, retirements, deactivate};
  std::unique_ptr<PreparedEngine> engine;

  explicit Fixture(std::string_view name)
      : root(testTemporaryRoot() / name), source(root / "main.onda"),
        asset(root / "data.ondabuffer") {
    std::filesystem::create_directories(root);
    worker.configure(48'000.0, 8);
  }

  bool
  writeSample(float sample,
              const std::filesystem::path &filename = "data.ondabuffer") const {
    const std::array samples{sample};
    Diagnostic diagnostic;
    const auto bytes =
        encodeF32BufferAsset(samples, 1, 1, 48'000.0F, diagnostic);
    std::ofstream file(root / filename, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return !bytes.empty() && file.good();
  }

  bool adopt(float expected) {
    static_cast<void>(worker.waitForPreparation());
    auto replacement = std::unique_ptr<PreparedEngine>{replacements.tryPop()};
    if (!replacement) {
      std::cerr << "cache preparation failed: " << worker.status().message
                << '\n';
      return false;
    }
    if (!renders(*replacement, expected))
      return false;
    worker.setActiveGeneration(replacement->buildGeneration());
    engine = std::move(replacement);
    return true;
  }

  bool checkpointRenders(float expected) {
    auto restored =
        PreparedEngine::build(worker.persistedProjectState().projectImage,
                              Product::effect, 48'000.0, 8);
    return restored.engine && renders(*restored.engine, expected);
  }

  bool preparationFails(float retained) {
    static_cast<void>(worker.waitForPreparation());
    const auto unexpected =
        std::unique_ptr<PreparedEngine>{replacements.tryPop()};
    if (unexpected || !worker.status().hasError) {
      std::cerr << "invalid buffer preparation unexpectedly succeeded\n";
      return false;
    }
    return renders(*engine, retained);
  }
};

bool sourceEditBeforeBufferChange() {
  for (const bool imported : {false, true}) {
    for (const bool clear : {false, true}) {
      Fixture fixture(std::string{"source-edit-"} +
                      (imported ? "import" : "entry") +
                      (clear ? "-clear" : "-bind"));
      const auto dependency = fixture.root / "dependency.onda";
      const auto writeSource = [&](int value) {
        if (imported &&
            !writeText(dependency, "def value() { return " +
                                       std::to_string(value) + ".0 }\n"))
          return false;
        const auto expression =
            imported ? std::string{"value()"} : std::to_string(value) + ".0";
        return writeText(fixture.source,
                         std::string{imported ? "import dependency\n" : ""} +
                             "buffers { a: buffer<f32> }\nouts { out1, out2 }\n"
                             "sample { out1 = " +
                             expression + " + a[0]; out2 = " + expression +
                             " + a[0] }\n");
      };
      if (!writeSource(1) || !fixture.writeSample(0.25F))
        return false;
      fixture.worker.loadWithBufferBindings(fixture.source,
                                            {{"a", fixture.asset}}, false);
      if (!fixture.adopt(1.25F))
        return false;

      const auto changedPath = imported ? dependency : fixture.source;
      const auto modified = std::filesystem::last_write_time(changedPath);
      if (!writeSource(2))
        return false;
      // Equal size and mtime still require content validation before reuse.
      std::filesystem::last_write_time(changedPath, modified);
      if (clear)
        fixture.worker.clearBuffer("a");
      else
        fixture.worker.bindBufferFile("a", fixture.asset);
      const auto expected = clear ? 2.0F : 2.25F;
      if (!fixture.adopt(expected) || !fixture.checkpointRenders(expected))
        return false;
    }
  }
  return true;
}

bool sharedFileEditBeforeRebind() {
  enum class SourceEdit { none, valid, invalid };
  struct Case {
    const char *name;
    bool failedPreparation;
    SourceEdit sourceEdit;
  };
  const Case cases[]{
      {"shared-file-edit", false, SourceEdit::none},
      {"shared-file-after-failure", true, SourceEdit::none},
      {"shared-file-after-source-edit-and-failure", true, SourceEdit::valid},
      {"shared-file-after-compile-failure", true, SourceEdit::invalid},
  };
  for (const auto &[name, failedPreparation, sourceEdit] : cases) {
    Fixture fixture(name);
    if (!writeText(fixture.source,
                   "buffers { a: buffer<f32>, b: buffer<f32> }\n"
                   "outs { out1, out2 }\n"
                   "sample { out1 = a[0] + b[0]; out2 = a[0] + b[0] }\n") ||
        !fixture.writeSample(0.25F))
      return false;
    fixture.worker.loadWithBufferBindings(
        fixture.source, {{"a", fixture.asset}, {"b", fixture.asset}}, false);
    if (!fixture.adopt(0.5F))
      return false;
    const auto modified = std::filesystem::last_write_time(fixture.asset);
    if (!fixture.writeSample(0.75F))
      return false;
    std::filesystem::last_write_time(fixture.asset, modified);
    if (failedPreparation) {
      if (sourceEdit != SourceEdit::none) {
        std::ofstream source(fixture.source, std::ios::app);
        source << (sourceEdit == SourceEdit::valid
                       ? "\n# Recompile before attempting the bindings.\n"
                       : "\ninvalid syntax\n");
        if (!source.good())
          return false;
      }
      fixture.worker.bindBufferFile("a", fixture.root / "missing.ondabuffer");
      if (!fixture.preparationFails(0.5F) || !fixture.checkpointRenders(0.5F))
        return false;
      // A failed attempt advances watches, but cannot validate cached samples.
      fixture.worker.clearBuffer("a");
    } else {
      fixture.worker.bindBufferFile("a", fixture.asset);
    }
    const auto expected = failedPreparation ? 0.75F : 1.5F;
    if (!fixture.adopt(expected) || !fixture.checkpointRenders(expected))
      return false;
  }
  return true;
}

bool failedDecodeBeforePeerClear() {
  for (const bool fromImage : {false, true}) {
    Fixture fixture(fromImage ? "failed-image-buffer" : "failed-disk-buffer");
    if (!writeText(fixture.source,
                   "buffers { a: buffer<f32>, b: buffer<f32> }\n"
                   "outs { out1, out2 }\n"
                   "sample { out1 = a[0] + b[0]; out2 = a[0] + b[0] }\n") ||
        !fixture.writeSample(0.25F))
      return false;
    fixture.worker.loadWithBufferBindings(
        fixture.source, {{"a", fixture.asset}, {"b", fixture.asset}}, false);
    if (!fixture.adopt(0.5F))
      return false;
    if (fromImage) {
      const auto checkpoint = fixture.worker.persistedProjectState();
      std::filesystem::remove(fixture.source);
      fixture.worker.restore(
          checkpoint.path, checkpoint.projectImage, checkpoint.bufferBindings,
          Worker::ExistingEnginePolicy::deactivateImmediately);
      if (!fixture.adopt(0.5F) || !fixture.worker.status().usingProjectImage)
        return false;
    }
    // Decode b from a separate override, including after image restoration.
    const auto peer = fixture.root / "peer.ondabuffer";
    std::filesystem::copy_file(fixture.asset, peer);
    fixture.worker.bindBufferFile("b", peer);
    if (!fixture.adopt(0.5F))
      return false;
    const auto checkpoint = fixture.worker.persistedProjectState().projectImage;
    if (!writeText(peer, "invalid buffer file"))
      return false;
    fixture.worker.bindBufferFile("b", peer);
    if (!fixture.preparationFails(0.5F))
      return false;
    fixture.worker.clearBuffer("a");
    if (!fixture.preparationFails(0.5F) ||
        fixture.worker.persistedProjectState().projectImage != checkpoint)
      return false;
    // A repaired file must be decoded and saved, rather than reusing 0.25.
    if (!fixture.writeSample(0.75F, "peer.ondabuffer"))
      return false;
    fixture.worker.bindBufferFile("b", peer);
    if (!fixture.adopt(0.75F) || !fixture.checkpointRenders(0.75F))
      return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const test::Case cases[]{
      {"SourceEditBeforeBufferChange", sourceEditBeforeBufferChange},
      {"SharedFileEditBeforeRebind", sharedFileEditBeforeRebind},
      {"FailedDecodeBeforePeerClear", failedDecodeBeforePeerClear},
  };
  return test::runCases(argc, argv, cases);
}
