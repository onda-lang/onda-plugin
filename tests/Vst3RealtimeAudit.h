#pragma once

#include <cstdint>

struct Vst3RealtimeCounts {
  std::uint64_t locks{};
  std::uint64_t allocations{};
  std::uint64_t frees{};
};

extern "C" {
void onda_vst3_audit_begin() noexcept;
Vst3RealtimeCounts onda_vst3_audit_end() noexcept;
}
