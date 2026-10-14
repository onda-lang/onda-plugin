#pragma once
#include "AuditCounters.h"
#include "Processor.h"

namespace test {
bool exerciseRealtimeAuditInstrumentation();

template <typename Callback> bool auditRealtime(Callback callback) {
  allocation_audit::count.store(0, std::memory_order_relaxed);
  lock_audit::count.store(0, std::memory_order_relaxed);
  allocation_audit::enabled = lock_audit::enabled = true;
  callback();
  allocation_audit::enabled = lock_audit::enabled = false;
  return allocation_audit::count.load(std::memory_order_relaxed) == 0 &&
         lock_audit::count.load(std::memory_order_relaxed) == 0;
}

inline bool captureRealtime(onda::plugin::Processor &processor,
                            std::uint32_t id,
                            onda::plugin::AutomationQueue queue, int frames) {
  return auditRealtime(
      [&] { processor.captureHostAutomation(id, queue, frames); });
}

inline bool processRealtime(onda::plugin::Processor &processor,
                            juce::AudioBuffer<float> &audio,
                            juce::MidiBuffer &midi,
                            const bool bypassed = false) {
  return auditRealtime([&] {
    processor.publishHostAutomationTargets();
    if (bypassed)
      processor.processBlockBypassed(audio, midi);
    else
      processor.processBlock(audio, midi);
    processor.clearHostAutomation();
  });
}
} // namespace test
