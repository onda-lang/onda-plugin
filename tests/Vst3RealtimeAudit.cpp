#include "Vst3RealtimeAudit.h"

#include <bit>
#include <cerrno>
#include <cstddef>
#include <dlfcn.h>
#include <pthread.h>

// LD_PRELOAD intercepts the exported plugin's calls as well as the host's.
// glibc's allocation entry points avoid recursive dlsym during startup.
extern "C" {
void *__libc_malloc(std::size_t) noexcept;
void *__libc_calloc(std::size_t, std::size_t) noexcept;
void *__libc_realloc(void *, std::size_t) noexcept;
void *__libc_memalign(std::size_t, std::size_t) noexcept;
void *__libc_valloc(std::size_t) noexcept;
void *__libc_pvalloc(std::size_t) noexcept;
void __libc_free(void *) noexcept;
}

namespace {
thread_local bool enabled{};
thread_local Vst3RealtimeCounts counts;

void recordAllocation() noexcept {
  if (enabled)
    ++counts.allocations;
}
} // namespace

extern "C" void onda_vst3_audit_begin() noexcept {
  counts = {};
  enabled = true;
}

extern "C" Vst3RealtimeCounts onda_vst3_audit_end() noexcept {
  enabled = false;
  return counts;
}

extern "C" int pthread_mutex_lock(pthread_mutex_t *mutex) noexcept {
  // Other libraries can take a lock before this library's constructors run.
  static const auto mutexLock = reinterpret_cast<int (*)(pthread_mutex_t *)>(
      dlsym(RTLD_NEXT, "pthread_mutex_lock"));
  if (enabled)
    ++counts.locks;
  return mutexLock(mutex);
}

extern "C" void *malloc(const std::size_t size) noexcept {
  recordAllocation();
  return __libc_malloc(size);
}

extern "C" void *calloc(const std::size_t count,
                        const std::size_t size) noexcept {
  recordAllocation();
  return __libc_calloc(count, size);
}

extern "C" void *realloc(void *pointer, const std::size_t size) noexcept {
  recordAllocation();
  return __libc_realloc(pointer, size);
}

extern "C" void *aligned_alloc(const std::size_t alignment,
                               const std::size_t size) noexcept {
  recordAllocation();
  // __libc_memalign accepts looser alignments than the public C/POSIX APIs.
  if (!std::has_single_bit(alignment)) {
    errno = EINVAL;
    return nullptr;
  }
  return __libc_memalign(alignment, size);
}

extern "C" int posix_memalign(void **pointer, const std::size_t alignment,
                              const std::size_t size) noexcept {
  recordAllocation();
  if (alignment < sizeof(void *) || !std::has_single_bit(alignment))
    return EINVAL;
  auto *const allocation = __libc_memalign(alignment, size);
  if (allocation == nullptr)
    return ENOMEM;
  *pointer = allocation;
  return 0;
}

extern "C" void *memalign(const std::size_t alignment,
                          const std::size_t size) noexcept {
  recordAllocation();
  return __libc_memalign(alignment, size);
}

extern "C" void *valloc(const std::size_t size) noexcept {
  recordAllocation();
  return __libc_valloc(size);
}

extern "C" void *pvalloc(const std::size_t size) noexcept {
  recordAllocation();
  return __libc_pvalloc(size);
}

extern "C" void free(void *pointer) noexcept {
  if (pointer != nullptr && enabled)
    ++counts.frees;
  __libc_free(pointer);
}
