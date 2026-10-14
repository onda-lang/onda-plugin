#include "AuditCounters.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

#if defined(__linux__)
#include <pthread.h>
#endif
#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace allocation_audit {
thread_local bool enabled{};
std::atomic<std::uint64_t> count{};

static void record() noexcept {
  if (enabled)
    count.fetch_add(1, std::memory_order_relaxed);
}

static void *allocateAligned(const std::size_t size,
                             const std::size_t alignment) {
  const auto requested = std::max(size, std::size_t{1});
#if defined(_MSC_VER)
  return _aligned_malloc(requested, alignment);
#else
  // Darwin rejects small alignments even when malloc would satisfy them.
  const auto effectiveAlignment =
      std::max(alignment, alignof(std::max_align_t));
  if (requested >
      std::numeric_limits<std::size_t>::max() - (effectiveAlignment - 1U))
    return nullptr;
  const auto padded =
      ((requested + effectiveAlignment - 1U) / effectiveAlignment) *
      effectiveAlignment;
  return std::aligned_alloc(effectiveAlignment, padded);
#endif
}

static void freeMemory(void *memory) noexcept {
#if !defined(__linux__)
  if (memory != nullptr)
    record();
#endif
  std::free(memory);
}

static void freeAligned(void *memory) noexcept {
#if defined(_MSC_VER)
  if (memory != nullptr)
    record();
  _aligned_free(memory);
#else
  freeMemory(memory);
#endif
}
} // namespace allocation_audit

namespace lock_audit {
thread_local bool enabled{};
std::atomic<std::uint64_t> count{};
} // namespace lock_audit

#if defined(__linux__)
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (lock_audit::enabled)
    lock_audit::count.fetch_add(1, std::memory_order_relaxed);
  return __real_pthread_mutex_lock(mutex);
}
extern "C" void *__real_malloc(std::size_t);
extern "C" void *__real_calloc(std::size_t, std::size_t);
extern "C" void *__real_realloc(void *, std::size_t);
extern "C" void __real_free(void *);
extern "C" void *__real_aligned_alloc(std::size_t, std::size_t);
extern "C" int __real_posix_memalign(void **, std::size_t, std::size_t);
extern "C" void *__real_memalign(std::size_t, std::size_t);
extern "C" void *__real_valloc(std::size_t);
extern "C" void *__real_pvalloc(std::size_t);
extern "C" void *__real_reallocarray(void *, std::size_t, std::size_t);
extern "C" char *__real_strdup(const char *);
extern "C" char *__real_strndup(const char *, std::size_t);
extern "C" void *__wrap_malloc(const std::size_t size) {
  allocation_audit::record();
  return __real_malloc(size);
}
extern "C" void *__wrap_calloc(const std::size_t count,
                               const std::size_t size) {
  allocation_audit::record();
  return __real_calloc(count, size);
}
extern "C" void *__wrap_realloc(void *pointer, const std::size_t size) {
  allocation_audit::record();
  return __real_realloc(pointer, size);
}
extern "C" void __wrap_free(void *pointer) {
  if (pointer != nullptr)
    allocation_audit::record();
  __real_free(pointer);
}
extern "C" void *__wrap_aligned_alloc(const std::size_t alignment,
                                      const std::size_t size) {
  allocation_audit::record();
  return __real_aligned_alloc(alignment, size);
}
extern "C" int __wrap_posix_memalign(void **pointer,
                                     const std::size_t alignment,
                                     const std::size_t size) {
  allocation_audit::record();
  return __real_posix_memalign(pointer, alignment, size);
}
extern "C" void *__wrap_memalign(const std::size_t alignment,
                                 const std::size_t size) {
  allocation_audit::record();
  return __real_memalign(alignment, size);
}
extern "C" void *__wrap_valloc(const std::size_t size) {
  allocation_audit::record();
  return __real_valloc(size);
}
extern "C" void *__wrap_pvalloc(const std::size_t size) {
  allocation_audit::record();
  return __real_pvalloc(size);
}
extern "C" void *__wrap_reallocarray(void *pointer, const std::size_t count,
                                     const std::size_t size) {
  allocation_audit::record();
  return __real_reallocarray(pointer, count, size);
}
extern "C" char *__wrap_strdup(const char *text) {
  allocation_audit::record();
  return __real_strdup(text);
}
extern "C" char *__wrap_strndup(const char *text, const std::size_t size) {
  allocation_audit::record();
  return __real_strndup(text, size);
}
#if __GLIBC_PREREQ(2, 43)
extern "C" void __real_free_sized(void *, std::size_t);
extern "C" void __real_free_aligned_sized(void *, std::size_t, std::size_t);
extern "C" void __wrap_free_sized(void *pointer, const std::size_t size) {
  if (pointer != nullptr)
    allocation_audit::record();
  __real_free_sized(pointer, size);
}
extern "C" void __wrap_free_aligned_sized(void *pointer,
                                          const std::size_t alignment,
                                          const std::size_t size) {
  if (pointer != nullptr)
    allocation_audit::record();
  __real_free_aligned_sized(pointer, alignment, size);
}
#endif
#endif

void *operator new(const std::size_t size) {
  allocation_audit::record();
  if (auto *memory = std::malloc(std::max(size, std::size_t{1})))
    return memory;
  throw std::bad_alloc{};
}

void *operator new[](const std::size_t size) { return ::operator new(size); }

void *operator new(const std::size_t size, const std::align_val_t alignment) {
  allocation_audit::record();
  if (auto *memory = allocation_audit::allocateAligned(
          size, static_cast<std::size_t>(alignment)))
    return memory;
  throw std::bad_alloc{};
}

void *operator new[](const std::size_t size, const std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void *operator new(const std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](const std::size_t size,
                     const std::nothrow_t &tag) noexcept {
  return ::operator new(size, tag);
}

void *operator new(const std::size_t size, const std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](const std::size_t size, const std::align_val_t alignment,
                     const std::nothrow_t &tag) noexcept {
  return ::operator new(size, alignment, tag);
}

void operator delete(void *memory) noexcept {
  allocation_audit::freeMemory(memory);
}
void operator delete[](void *memory) noexcept {
  allocation_audit::freeMemory(memory);
}
void operator delete(void *memory, std::size_t) noexcept {
  allocation_audit::freeMemory(memory);
}
void operator delete[](void *memory, std::size_t) noexcept {
  allocation_audit::freeMemory(memory);
}
void operator delete(void *memory, std::align_val_t) noexcept {
  allocation_audit::freeAligned(memory);
}
void operator delete[](void *memory, std::align_val_t) noexcept {
  allocation_audit::freeAligned(memory);
}
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
  allocation_audit::freeAligned(memory);
}
void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept {
  allocation_audit::freeAligned(memory);
}
void operator delete(void *memory, const std::nothrow_t &) noexcept {
  allocation_audit::freeMemory(memory);
}
void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  allocation_audit::freeMemory(memory);
}
void operator delete(void *memory, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  allocation_audit::freeAligned(memory);
}
void operator delete[](void *memory, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  allocation_audit::freeAligned(memory);
}
