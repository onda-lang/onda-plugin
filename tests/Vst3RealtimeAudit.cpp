#include "Vst3RealtimeAudit.h"

#include <cstddef>
#include <dlfcn.h>
#include <pthread.h>

// LD_PRELOAD intercepts the exported plugin's calls as well as the host's.
// glibc's allocation entry points avoid recursive dlsym during startup.
extern "C" {
void *__libc_malloc(std::size_t) noexcept;
void *__libc_calloc(std::size_t, std::size_t) noexcept;
void *__libc_realloc(void *, std::size_t) noexcept;
void __libc_free(void *) noexcept;
}

namespace {
thread_local bool enabled{};
thread_local Vst3RealtimeCounts counts;
int (*mutexLock)(pthread_mutex_t *);

__attribute__((constructor)) void initialize() {
  mutexLock = reinterpret_cast<decltype(mutexLock)>(
      dlsym(RTLD_NEXT, "pthread_mutex_lock"));
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
  if (enabled)
    ++counts.locks;
  return mutexLock(mutex);
}

extern "C" void *malloc(const std::size_t size) noexcept {
  if (enabled)
    ++counts.allocations;
  return __libc_malloc(size);
}

extern "C" void *calloc(const std::size_t count,
                        const std::size_t size) noexcept {
  if (enabled)
    ++counts.allocations;
  return __libc_calloc(count, size);
}

extern "C" void *realloc(void *pointer, const std::size_t size) noexcept {
  if (enabled)
    ++counts.allocations;
  return __libc_realloc(pointer, size);
}

extern "C" void free(void *pointer) noexcept {
  if (enabled && pointer != nullptr)
    ++counts.frees;
  __libc_free(pointer);
}
