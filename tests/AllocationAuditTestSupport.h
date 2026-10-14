#pragma once

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(__linux__)
#include <malloc.h>
#include <unistd.h>
#endif

namespace test {
struct AllocationAuditCase {
  const char *name;
  void *(*allocate)();
  void (*release)(void *);
  std::size_t alignment;
};

inline auto allocationAuditCases() {
  constexpr std::size_t alignment = 64;
  return std::array{
#if defined(__linux__)
      AllocationAuditCase{"malloc", [] { return std::malloc(128); }, std::free,
                          1},
      AllocationAuditCase{"calloc", [] { return std::calloc(4, 32); },
                          std::free, 1},
      AllocationAuditCase{"realloc(nullptr)",
                          [] { return std::realloc(nullptr, 128); }, std::free,
                          1},
      AllocationAuditCase{"aligned_alloc",
                          [] { return std::aligned_alloc(alignment, 128); },
                          std::free, alignment},
      AllocationAuditCase{"posix_memalign",
                          [] {
                            void *pointer{};
                            return posix_memalign(&pointer, alignment, 128) == 0
                                       ? pointer
                                       : nullptr;
                          },
                          std::free, alignment},
      AllocationAuditCase{"memalign", [] { return memalign(alignment, 128); },
                          std::free, alignment},
      AllocationAuditCase{"valloc", [] { return valloc(128); }, std::free,
                          static_cast<std::size_t>(getpagesize())},
      AllocationAuditCase{"pvalloc", [] { return pvalloc(128); }, std::free,
                          static_cast<std::size_t>(getpagesize())},
#endif
      AllocationAuditCase{"operator new", [] { return ::operator new(128); },
                          [](void *p) { ::operator delete(p); }, 1},
      AllocationAuditCase{"operator new[]",
                          [] { return ::operator new[](128); },
                          [](void *p) { ::operator delete[](p); }, 1},
      AllocationAuditCase{
          "aligned operator new",
          [] { return ::operator new(128, std::align_val_t{alignment}); },
          [](void *p) { ::operator delete(p, std::align_val_t{alignment}); },
          alignment},
      AllocationAuditCase{
          "aligned operator new[]",
          [] { return ::operator new[](128, std::align_val_t{alignment}); },
          [](void *p) { ::operator delete[](p, std::align_val_t{alignment}); },
          alignment},
      AllocationAuditCase{
          "sized operator delete", [] { return ::operator new(128); },
          [](void *p) { ::operator delete(p, std::size_t{128}); }, 1},
      AllocationAuditCase{
          "sized operator delete[]", [] { return ::operator new[](128); },
          [](void *p) { ::operator delete[](p, std::size_t{128}); }, 1},
      AllocationAuditCase{
          "aligned sized operator delete",
          [] { return ::operator new(128, std::align_val_t{alignment}); },
          [](void *p) {
            ::operator delete(p, std::size_t{128}, std::align_val_t{alignment});
          },
          alignment},
      AllocationAuditCase{
          "aligned sized operator delete[]",
          [] { return ::operator new[](128, std::align_val_t{alignment}); },
          [](void *p) {
            ::operator delete[](p, std::size_t{128},
                                std::align_val_t{alignment});
          },
          alignment},
      AllocationAuditCase{"nothrow operator new",
                          [] { return ::operator new(128, std::nothrow); },
                          [](void *p) { ::operator delete(p, std::nothrow); },
                          1},
      AllocationAuditCase{"nothrow operator new[]",
                          [] { return ::operator new[](128, std::nothrow); },
                          [](void *p) { ::operator delete[](p, std::nothrow); },
                          1},
      AllocationAuditCase{"aligned nothrow operator new",
                          [] {
                            return ::operator new(
                                128, std::align_val_t{alignment}, std::nothrow);
                          },
                          [](void *p) {
                            ::operator delete(p, std::align_val_t{alignment},
                                              std::nothrow);
                          },
                          alignment},
      AllocationAuditCase{"aligned nothrow operator new[]",
                          [] {
                            return ::operator new[](
                                128, std::align_val_t{alignment}, std::nothrow);
                          },
                          [](void *p) {
                            ::operator delete[](p, std::align_val_t{alignment},
                                                std::nothrow);
                          },
                          alignment},
#if defined(__linux__)
      // These libc helpers should reach the intercepted allocation functions.
      AllocationAuditCase{"reallocarray",
                          [] { return reallocarray(nullptr, 4, 32); },
                          std::free, 1},
      AllocationAuditCase{"strdup",
                          [] { return static_cast<void *>(strdup("audit")); },
                          std::free, 1},
      AllocationAuditCase{
          "strndup", [] { return static_cast<void *>(strndup("audit", 3)); },
          std::free, 1},
#if __GLIBC_PREREQ(2, 43)
      AllocationAuditCase{"free_sized", [] { return std::malloc(128); },
                          [](void *p) { free_sized(p, 128); }, 1},
      AllocationAuditCase{
          "free_aligned_sized",
          [] { return std::aligned_alloc(alignment, 128); },
          [](void *p) { free_aligned_sized(p, alignment, 128); }, alignment},
#endif
#endif
  };
}
} // namespace test
