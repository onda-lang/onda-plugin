#pragma once

#include <atomic>
#include <cstdint>

namespace allocation_audit {
extern thread_local bool enabled;
extern std::atomic<std::uint64_t> count;
} // namespace allocation_audit
namespace lock_audit {
extern thread_local bool enabled;
extern std::atomic<std::uint64_t> count;
} // namespace lock_audit
