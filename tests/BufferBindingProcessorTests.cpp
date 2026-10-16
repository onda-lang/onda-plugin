#include "BufferBindingProcessorTests.h"

#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"

#include <fstream>
#include <iostream>

bool exerciseTypedBufferState() {
  using namespace onda::plugin;
  const auto root = testTemporaryRoot() / "typed-buffer-state";
  std::filesystem::create_directories(root);
  const auto source = root / "main.onda";
  const auto first = root / "first.ondabuffer";
  const auto second = root / "second.ondabuffer";
  const auto writeAsset = [](const std::filesystem::path &path,
                             std::int64_t value) {
    const std::array samples{value};
    Diagnostic diagnostic;
    const auto bytes =
        encodeBufferAsset(ONDA_PRIMITIVE_I64, std::as_bytes(std::span{samples}),
                          1, 1, 48'000.0F, diagnostic);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return !bytes.empty() && file.good();
  };
  const auto renders = [](Processor &processor, float expected) {
    juce::AudioBuffer<float> audio(
        std::max(pluginInputChannels, pluginOutputChannels), 8);
    juce::MidiBuffer midi;
    audio.clear();
    if (!test::processRealtime(processor, audio, midi) ||
        !test::withinTolerance(audio.getSample(0, 0) - expected, 1.0e-6F)) {
      std::cerr << "typed buffer state rendered " << audio.getSample(0, 0)
                << "; expected " << expected << ": "
                << processor.workerStatus().message << '\n';
      return false;
    }
    return true;
  };
  for (const auto product : {Product::effect, Product::instrument}) {
    {
      std::ofstream file(source);
      file << "buffers { a: buffer<i64>, b: buffer<i64> }\n"
              "params { gain = 0.25 {0.0, 1.0} }\nouts { out1 }\n"
              "event note_on(id: i32, channel: i32, key: i32, velocity: f32) "
              "{}\n"
              "event note_off(id: i32, channel: i32, key: i32, velocity: f32) "
              "{}\n"
              "sample { out1 = f32(a[0]) + f32(b[0]) + gain }\n";
    }
    if (!writeAsset(first, 3) || !writeAsset(second, 7))
      return false;
    juce::MemoryBlock saved;
    {
      Processor processor(product);
      processor.setNonRealtime(true);
      processor.loadFile(juce::File(pathToJuce(source)));
      processor.bindBufferFile("a", juce::File(pathToJuce(first)));
      processor.bindBufferFile("b", juce::File(pathToJuce(second)));
      processor.prepareToPlay(48'000.0, 8);
      if (!renders(processor, 10.25F))
        return false;
      processor.getStateInformation(saved);
    }
    std::filesystem::remove(source);
    std::filesystem::remove(first);
    std::filesystem::remove(second);
    Processor restored(product);
    restored.setNonRealtime(true);
    restored.setStateInformation(saved.getData(),
                                 static_cast<int>(saved.getSize()));
    restored.prepareToPlay(48'000.0, 8);
    if (!restored.workerStatus().usingProjectImage ||
        !renders(restored, 10.25F))
      return false;
    restored.clearBuffer("a");
    juce::MemoryBlock pendingClear;
    restored.getStateInformation(pendingClear);
    Processor pending(product);
    pending.setNonRealtime(true);
    pending.setStateInformation(pendingClear.getData(),
                                static_cast<int>(pendingClear.getSize()));
    pending.prepareToPlay(48'000.0, 8);
    if (!renders(pending, 7.25F) || !pending.canExportProject()) {
      std::cerr << "a pending clear did not restore an exportable unbound "
                   "checkpoint\n";
      return false;
    }
    restored.prepareToPlay(48'000.0, 8);
    if (!renders(restored, 7.25F) || !restored.canExportProject())
      return false;
    if (!writeAsset(second, 11))
      return false;
    restored.bindBufferFile("b", juce::File(pathToJuce(second)));
    restored.prepareToPlay(48'000.0, 8);
    if (!renders(restored, 11.25F))
      return false;
    std::filesystem::remove(second);
    restored.requestReload();
    restored.prepareToPlay(48'000.0, 8);
    if (!writeAsset(first, 5))
      return false;
    restored.bindBufferFile("a", juce::File(pathToJuce(first)));
    restored.prepareToPlay(48'000.0, 8);
    if (!renders(restored, 16.25F) || !restored.canExportProject())
      return false;
    restored.bindBufferFile(
        "a", juce::File(pathToJuce(root / "missing.ondabuffer")));
    restored.prepareToPlay(48'000.0, 8);
    if (!restored.workerStatus().pendingBuffers || !renders(restored, 16.25F))
      return false;
    restored.clearBuffer("a");
    juce::MemoryBlock clearedState;
    restored.getStateInformation(clearedState);
    Processor cleared(product);
    cleared.setNonRealtime(true);
    cleared.setStateInformation(clearedState.getData(),
                                static_cast<int>(clearedState.getSize()));
    cleared.prepareToPlay(48'000.0, 8);
    if (!renders(cleared, 11.25F) || !cleared.canExportProject())
      return false;
    restored.prepareToPlay(48'000.0, 8);
    if (!renders(restored, 11.25F))
      return false;
    const auto exported =
        root / (product == Product::effect ? "effect" : "instrument");
    if (const auto error =
            restored.saveProjectAs(juce::File(pathToJuce(exported)));
        !error.empty()) {
      std::cerr << "typed state export failed: " << error << '\n';
      return false;
    }
  }
  return true;
}
