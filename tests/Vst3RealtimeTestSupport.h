#pragma once

#include "Vst3RealtimeAudit.h"

#include <iostream>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/vst/ivstaudioprocessor.h>
#if defined(__linux__)
#include <cstdlib>
#include <dlfcn.h>
#endif

namespace vst3_test {
inline Steinberg::tresult
processRealtime(Steinberg::Vst::IAudioProcessor &processor,
                Steinberg::Vst::ProcessData &data) {
#if defined(__linux__)
  static const auto begin = reinterpret_cast<decltype(&onda_vst3_audit_begin)>(
      dlsym(RTLD_DEFAULT, "onda_vst3_audit_begin"));
  static const auto end = reinterpret_cast<decltype(&onda_vst3_audit_end)>(
      dlsym(RTLD_DEFAULT, "onda_vst3_audit_end"));
  if (begin != nullptr && end != nullptr) {
    begin();
    const auto result = processor.process(data);
    Vst3RealtimeCounts counts;
    end(&counts);
    // JUCE retains one callback mutex. Managed automation must add no locks,
    // allocations or frees, including queue flushes and MIDI segment splits.
    if (counts.hasViolations(1)) {
      std::cerr << "VST3 realtime audit: " << counts.locks << " locks, "
                << counts.allocations << " allocations, " << counts.frees
                << " frees\n";
      return Steinberg::kResultFalse;
    }
    return result;
  }
  if (std::getenv("ONDA_REQUIRE_VST3_AUDIT") != nullptr) {
    std::cerr << "VST3 realtime audit library was not loaded\n";
    return Steinberg::kResultFalse;
  }
#endif
  return processor.process(data);
}

} // namespace vst3_test
