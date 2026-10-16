#include "BufferBindingTests.h"

#include "Engine.h"
#include "ProjectExport.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"
#include "TestWait.h"
#include "Worker.h"

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace onda::plugin;

bool renders(PreparedEngine &engine, float expected) {
  ParameterState parameters;
  std::array<float, 8> left{}, right{};
  const std::array outputs{left.data(), right.data()};
  const auto success =
      engine.process(nullptr, outputs.data(), 8, {}, parameters);
  if (!success || !test::withinTolerance(left[0] - expected, 1.0e-6F) ||
      !test::withinTolerance(right[0] - expected, 1.0e-6F)) {
    std::cerr << "buffers rendered " << left[0] << ", " << right[0]
              << "; expected " << expected << " for both\n";
    return false;
  }
  return true;
}

bool write(const std::filesystem::path &path,
           std::span<const std::uint8_t> bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

} // namespace

bool exerciseTypedBufferBindings() {
  struct Case {
    const char *type;
    BufferSamples samples;
    const char *expression;
    float expected;
  };
  const Case cases[]{
      {"bool",
       {.values = std::vector<std::uint8_t>{1, 0}},
       "f32(i32(data[0]))",
       1.0F},
      {"i32",
       {.values = std::vector<std::int32_t>{1234567, -7}},
       "f32(data[0] - 1234560)",
       7.0F},
      {"i64",
       {.values =
            std::vector<std::int64_t>{9007199254740993, -9007199254740995}},
       "f32(data[0] - 9007199254740992)",
       1.0F},
      {"f32", {.values = std::vector<float>{0.25F, -0.5F}}, "data[0]", 0.25F},
      {"f64",
       {.values = std::vector<double>{0.123456789012345, -0.5}},
       "f32(data[0] - 0.123456789012345)",
       0.0F},
  };
  for (const auto &testCase : cases) {
    const auto root =
        testTemporaryRoot() / (std::string{"typed-binding-"} + testCase.type);
    std::filesystem::create_directories(root);
    const auto source = root / "main.onda";
    const auto asset = root / "data.ondabuffer";
    {
      std::ofstream file(source);
      file << "buffers { data: buffer<" << testCase.type
           << "> }\nouts { out1, out2 }\nsample { value = "
           << testCase.expression << "; out1 = value; out2 = value }\n";
    }
    Diagnostic diagnostic;
    const auto encoded = encodeBufferAsset(testCase.samples.elementType(),
                                           testCase.samples.bytes(), 2, 1,
                                           48'000.0F, diagnostic);
    if (encoded.empty() || !write(asset, encoded))
      return false;
    const auto decoded = decodeAudioFile(asset);
    if (!decoded.audio ||
        decoded.audio->samples.values != testCase.samples.values) {
      std::cerr << testCase.type << " samples lost precision during decode\n";
      return false;
    }
    const std::array bindings{BufferFileBinding{.name = "data", .path = asset}};
    auto built =
        PreparedEngine::build(source, Product::effect, 48'000.0, 8, bindings);
    if (!built.engine || !renders(*built.engine, testCase.expected) ||
        !built.engine->bufferMappings()[0].waveform) {
      std::cerr << testCase.type
                << " binding failed: " << built.diagnostic.message << '\n';
      return false;
    }
    std::filesystem::remove(source);
    std::filesystem::remove(asset);
    auto retained =
        PreparedEngine::build(built.compilation, Product::effect, bindings,
                              std::nullopt, built.bufferFiles);
    if (!retained.engine || !renders(*retained.engine, testCase.expected) ||
        retained.bufferFiles[0].audio != built.bufferFiles[0].audio) {
      std::cerr << "unchanged file binding was reopened or copied\n";
      return false;
    }
    auto restored =
        PreparedEngine::build(built.projectImage, Product::effect, 48'000.0, 8);
    const auto exported = exportProject(built.projectImage, root / "exported");
    if (!restored.engine || !renders(*restored.engine, testCase.expected) ||
        !exported)
      return false;
    auto portable = PreparedEngine::build(exported.projectFile, Product::effect,
                                          48'000.0, 8);
    if (!portable.engine || !renders(*portable.engine, testCase.expected))
      return false;
    const std::array cleared{BufferFileBinding{.name = "data", .path = {}}};
    auto neutral =
        PreparedEngine::build(built.compilation, Product::effect, cleared);
    if (!neutral.engine ||
        neutral.compilation->program != built.compilation->program ||
        neutral.engine->bufferMappings()[0].loadedFrames != 0 ||
        neutral.engine->bufferMappings()[0].waveform)
      return false;
    // A cleared checkpoint exports no asset binding and restores unbound.
    auto empty = compileProjectImage(neutral.projectImage, 48'000.0, 8);
    if (!empty.program || !empty.projectBuffers.empty())
      return false;
  }
  return true;
}

bool exerciseBufferCompilationReuse() {
  const auto root = testTemporaryRoot() / "buffer-code-reuse";
  std::filesystem::create_directories(root);
  const auto source = root / "main.onda";
  const auto project = root / "project.ondaproject";
  {
    std::ofstream file(source);
    file
        << "config const Tiny: f64 = 0.0\nbuffers { bank: i64 {2} }\nouts { "
           "out1, out2 }\n"
           "sample { value = f32(bank[0][0]) + f32(bank[1][0]) + f32((Tiny - "
           "0.000012345678901234568) * 100000000000000000000.0); out1 = value; "
           "out2 = value }\n";
    std::ofstream manifest(project);
    manifest
        << R"({"entry":"main.onda","constants":{"Tiny":0.000012345678901234568},"buffers":{"bank":[{"inline":{"element":"i64","channels":1,"sample_rate":48000,"values":["3"]}},{"inline":{"element":"i64","channels":1,"sample_rate":48000,"values":["7"]}}]}})";
  }
  auto original = PreparedEngine::build(project, Product::effect, 48'000.0, 8);
  if (!original.engine || !renders(*original.engine, 10.0F)) {
    std::cerr << "project array default failed: " << original.diagnostic.message
              << '\n';
    return false;
  }
  const std::array cleared{BufferFileBinding{.name = "bank[0]", .path = {}}};
  auto changed =
      PreparedEngine::build(original.compilation, Product::effect, cleared);
  if (!changed.engine ||
      changed.compilation->program != original.compilation->program ||
      !renders(*changed.engine, 7.0F) || !renders(*original.engine, 10.0F)) {
    std::cerr << "clearing an array default modified its immutable program or "
                 "retained the default\n";
    return false;
  }
  auto restored =
      PreparedEngine::build(changed.projectImage, Product::effect, 48'000.0, 8);
  const auto exported = exportProject(changed.projectImage, root / "cleared");
  if (!restored.engine || !renders(*restored.engine, 7.0F) || !exported) {
    std::cerr << "cleared checkpoint failed: " << restored.diagnostic.message
              << "; export: " << exported.error << '\n';
    return false;
  }
  auto portable =
      PreparedEngine::build(exported.projectFile, Product::effect, 48'000.0, 8);
  if (!portable.engine)
    std::cerr << "cleared export failed: " << portable.diagnostic.message
              << '\n';
  return portable.engine && renders(*portable.engine, 7.0F);
}

bool exerciseSharedBufferFileReload() {
  const auto root = testTemporaryRoot() / "shared-buffer-file";
  std::filesystem::create_directories(root);
  const auto source = root / "main.onda";
  const auto asset = root / "shared.ondabuffer";
  {
    std::ofstream file(source);
    file << "buffers { a: buffer<f32>, b: buffer<f32> }\n"
            "outs { out1, out2 }\n"
            "sample { out1 = a[0]; out2 = b[0] }\n";
  }
  const auto writeSample = [&](float sample) {
    const std::array samples{sample};
    Diagnostic diagnostic;
    const auto bytes =
        encodeF32BufferAsset(samples, 1, 1, 48'000.0F, diagnostic);
    return !bytes.empty() && write(asset, bytes);
  };
  if (!writeSample(0.25F))
    return false;

  SpscSlot<PreparedEngine *> replacements, retirements;
  std::atomic<bool> deactivate{};
  Worker worker(Product::effect, replacements, retirements, deactivate);
  worker.configure(48'000.0, 8);
  worker.loadWithBufferBindings(source, {{"a", asset}, {"b", asset}}, false);
  static_cast<void>(worker.waitForPreparation());
  std::unique_ptr<PreparedEngine> initial{replacements.tryPop()};
  if (!initial || !renders(*initial, 0.25F)) {
    std::cerr << "shared buffer preparation failed: " << worker.status().message
              << '\n';
    return false;
  }
  worker.setActiveGeneration(initial->buildGeneration());

  if (!writeSample(0.75F))
    return false;
  std::unique_ptr<PreparedEngine> updated;
  if (!test::waitUntil(
          [&] {
            updated.reset(replacements.tryPop());
            return updated != nullptr;
          },
          test::yieldToWorker)) {
    std::cerr << "shared buffer edit did not trigger a reload: "
              << worker.status().message << '\n';
    return false;
  }
  if (!renders(*updated, 0.75F) || !renders(*initial, 0.25F))
    return false;

  // The saved checkpoint must contain the new samples for both bindings.
  auto restored = PreparedEngine::build(
      worker.persistedProjectState().projectImage, Product::effect, 48'000.0, 8);
  if (!restored.engine) {
    std::cerr << "shared buffer checkpoint failed: "
              << restored.diagnostic.message << '\n';
    return false;
  }
  return renders(*restored.engine, 0.75F);
}
