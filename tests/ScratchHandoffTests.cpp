#include "ScratchHandoffTests.h"

#include "JucePath.h"
#include "ProcessorTestSupport.h"
#include "RealtimeAudit.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

bool exerciseScratchHandoff() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "scratch-handoff.onda")};
  if (!source.replaceWithText(
          "outs { out1, out2 }\nsample { out1 = 0.25; out2 = 0.25 }\n"))
    return false;
  Processor processor(Product::effect);
  processor.prepareToPlay(48'000.0, 8);
  if (!processor.setPreferredBlockSize(3))
    return false;
  processor.loadFile(source);
  processor.prepareToPlay(48'000.0, 8);
  const auto generation = processor.workerStatus().engineGeneration;
  std::atomic<bool> done{}, succeeded{true};
  const auto deadline = std::chrono::steady_clock::now() + test::waitTimeout;
  std::thread audioThread([&] {
    juce::AudioBuffer<float> small(2, 8);
    juce::MidiBuffer midi;
    const auto render = [&](juce::AudioBuffer<float> &audio) {
      for (int channel = 0; channel < 2; ++channel)
        std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(),
                    0.5F);
      return test::processRealtime(processor, audio, midi);
    };
    for (int capacity = 16; capacity <= 4096 && succeeded.load();
         capacity *= 2) {
      juce::AudioBuffer<float> large(2, capacity);
      auto ready = false;
      while (!ready && succeeded.load() &&
             std::chrono::steady_clock::now() < deadline) {
        if (!render(large) || !render(small)) {
          succeeded.store(false);
          break;
        }
        // Small callbacks keep rendering throughout off-thread preparation;
        // the new larger bound may use dry fallback until its handoff arrives.
        for (int channel = 0; channel < 2; ++channel)
          for (int frame = 0; frame < 8; ++frame)
            if (!test::withinTolerance(small.getSample(channel, frame) - 0.25F,
                                       1.0e-6F))
              succeeded.store(false);
        ready = test::withinTolerance(large.getSample(0, 0) - 0.25F, 1.0e-6F);
        std::this_thread::yield();
      }
      if (!ready)
        succeeded.store(false);
    }
    done.store(true);
  });
  while (!done.load()) {
    test::service(processor);
    std::this_thread::yield();
  }
  audioThread.join();
  test::service(processor);
  if (!succeeded.load() || processor.hostBlockSize() != 4096 ||
      processor.ondaBlockSize() != 3 ||
      processor.workerStatus().engineGeneration != generation) {
    std::cerr << "concurrent scratch handoff interrupted rendering, allocated, "
                 "locked, or rebuilt the fixed-block engine\n";
    return false;
  }
  return true;
}
