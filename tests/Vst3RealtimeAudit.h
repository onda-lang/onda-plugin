#pragma once

#include <cstdint>

struct Vst3RealtimeCounts {
  std::uint64_t locks{};
  std::uint64_t allocations{};
  std::uint64_t frees{};

  [[nodiscard]] bool
  hasViolations(const std::uint64_t allowedLocks = 0) const noexcept {
    return locks > allowedLocks || allocations != 0 || frees != 0;
  }
};

extern "C" {
void onda_vst3_audit_begin() noexcept;
void onda_vst3_audit_end(Vst3RealtimeCounts *result) noexcept;
}
