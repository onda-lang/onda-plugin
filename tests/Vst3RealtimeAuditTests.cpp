#include "AllocationAuditTestSupport.h"
#include "Vst3RealtimeAudit.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <malloc.h>
#include <new>
#include <pthread.h>
#include <thread>
#include <unistd.h>

namespace {
void lockAndUnlock() {
  pthread_mutex_t mutex{};
  if (pthread_mutex_init(&mutex, nullptr) != 0 ||
      pthread_mutex_lock(&mutex) != 0 ||
      pthread_mutex_trylock(&mutex) != EBUSY ||
      pthread_mutex_unlock(&mutex) != 0 || pthread_mutex_destroy(&mutex) != 0)
    std::abort();
}

// Executable preinitializers run before shared-library constructors. This
// exercises a lock taken before the preloaded auditor can initialize itself.
void (*const preinitialize)()
    __attribute__((section(".preinit_array"), used)) = lockAndUnlock;

bool check(const bool condition, const char *message) {
  if (!condition)
    std::fprintf(stderr, "%s\n", message);
  return condition;
}

bool checkCounts(const char *name, const Vst3RealtimeCounts actual,
                 const Vst3RealtimeCounts expected) {
  if (actual.locks == expected.locks &&
      actual.allocations == expected.allocations &&
      actual.frees == expected.frees)
    return true;
  std::fprintf(stderr,
               "%s: expected locks/allocations/frees %" PRIu64 "/%" PRIu64
               "/%" PRIu64 ", got %" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n",
               name, expected.locks, expected.allocations, expected.frees,
               actual.locks, actual.allocations, actual.frees);
  return false;
}

Vst3RealtimeCounts endAudit() noexcept {
  Vst3RealtimeCounts counts;
  onda_vst3_audit_end(&counts);
  return counts;
}

template <typename Operation> Vst3RealtimeCounts measure(Operation operation) {
  onda_vst3_audit_begin();
  operation();
  return endAudit();
}

bool checkAllocations() {
  bool succeeded = true;
  for (const auto &test : test::allocationAuditCases()) {
    void *pointer{};
    // Keep allocation and deallocation in separate intervals: a later free
    // must never conceal a missed allocation during an audio callback.
    const auto allocation = measure([&] { pointer = test.allocate(); });
    succeeded &= checkCounts(test.name, allocation, {0, 1, 0});
    succeeded &= check(allocation.hasViolations(1),
                       "An allocation escaped the callback budget");
    succeeded &= check(
        pointer != nullptr &&
            reinterpret_cast<std::uintptr_t>(pointer) % test.alignment == 0,
        test.name);
    errno = EDOM;
    const auto release = measure([&] { test.release(pointer); });
    const auto preservedErrno = errno;
    succeeded &= checkCounts(test.name, release, {0, 0, 1});
    succeeded &= check(release.hasViolations(1),
                       "A deallocation escaped the callback budget");
    succeeded &= check(preservedErrno == EDOM, "Deallocation changed errno");
  }
  return succeeded;
}

bool checkAllocationSemantics() {
  bool succeeded = true;
  void *pointer{};
  const auto cleared = measure([&] { pointer = std::calloc(4, 32); });
  succeeded &= checkCounts("calloc clearing", cleared, {0, 1, 0});
  if (pointer == nullptr)
    return check(false, "calloc failed");
  const auto *bytes = static_cast<const unsigned char *>(pointer);
  for (std::size_t i = 0; i < 128; ++i)
    succeeded &= check(bytes[i] == 0, "calloc did not clear memory");
  std::memset(pointer, 0x5a, 128);
  void *grown{};
  const auto resized = measure([&] { grown = std::realloc(pointer, 256); });
  succeeded &= checkCounts("realloc preservation", resized, {0, 1, 0});
  if (grown != nullptr) {
    pointer = grown;
    bytes = static_cast<const unsigned char *>(pointer);
    for (std::size_t i = 0; i < 128; ++i)
      succeeded &= check(bytes[i] == 0x5a, "realloc changed existing bytes");
  } else {
    succeeded &= check(false, "realloc failed");
  }
  // Runtime sizes prevent compile-time allocation diagnostics/optimizations.
  volatile std::size_t oversized = std::numeric_limits<std::size_t>::max();
  errno = 0;
  const auto failedResize =
      measure([&] { grown = std::realloc(pointer, oversized); });
  const auto resizeErrno = errno;
  succeeded &= checkCounts("failed realloc", failedResize, {0, 1, 0});
  succeeded &= check(grown == nullptr && resizeErrno == ENOMEM &&
                         static_cast<unsigned char *>(pointer)[0] == 0x5a,
                     "Failed realloc must retain the original allocation");
  std::free(pointer);
  errno = 0;
  const auto overflow = measure([&] { pointer = std::calloc(oversized, 2); });
  const auto overflowErrno = errno;
  succeeded &= checkCounts("calloc overflow", overflow, {0, 1, 0});
  succeeded &= check(pointer == nullptr && overflowErrno == ENOMEM,
                     "calloc overflow must fail with ENOMEM");
  for (const std::size_t invalid : {0U, 3U}) {
    errno = 0;
    const auto invalidAligned =
        measure([&] { pointer = std::aligned_alloc(invalid, 128); });
    const auto alignedErrno = errno;
    succeeded &=
        checkCounts("invalid aligned_alloc", invalidAligned, {0, 1, 0});
    succeeded &= check(pointer == nullptr && alignedErrno == EINVAL,
                       "aligned_alloc must reject invalid alignment");
  }
  for (const auto invalid :
       {std::size_t{0}, std::size_t{3}, sizeof(void *) / 2}) {
    int sentinel{};
    pointer = &sentinel;
    errno = EDOM;
    int result{};
    const auto invalidPosix =
        measure([&] { result = posix_memalign(&pointer, invalid, 128); });
    const auto posixErrno = errno;
    succeeded &= checkCounts("invalid posix_memalign", invalidPosix, {0, 1, 0});
    succeeded &=
        check(result == EINVAL && pointer == &sentinel && posixErrno == EDOM,
              "posix_memalign must retain the pointer and errno on invalid "
              "alignment");
  }
  int sentinel{};
  pointer = &sentinel;
  int result{};
  const auto failedPosix = measure(
      [&] { result = posix_memalign(&pointer, sizeof(void *), oversized); });
  succeeded &= checkCounts("failed posix_memalign", failedPosix, {0, 1, 0});
  succeeded &= check(result == ENOMEM && pointer == &sentinel,
                     "Failed posix_memalign must retain its output pointer");
  for (const std::size_t alignment : {1U, 8U, 64U}) {
    const auto zero =
        measure([&] { pointer = std::aligned_alloc(alignment, 0); });
    succeeded &= checkCounts("zero aligned_alloc", zero, {0, 1, 0});
    succeeded &=
        check(pointer != nullptr &&
                  reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0,
              "Zero-size aligned allocation lost its alignment");
    std::free(pointer);
  }
  errno = 0;
  const auto pageOverflow = measure([&] { pointer = pvalloc(oversized); });
  const auto pageErrno = errno;
  succeeded &= checkCounts("pvalloc overflow", pageOverflow, {0, 1, 0});
  succeeded &= check(pointer == nullptr && pageErrno == ENOMEM,
                     "pvalloc overflow must fail with ENOMEM");
  return succeeded;
}

bool checkIntervals() {
  const auto oneLock = measure(lockAndUnlock);
  bool succeeded = checkCounts("mutex", oneLock, {1, 0, 0});
  succeeded &= check(oneLock.hasViolations() && !oneLock.hasViolations(1),
                     "The mutex allowance must apply only to mutex calls");
  const auto twoLocks = measure([] {
    lockAndUnlock();
    lockAndUnlock();
  });
  succeeded &= checkCounts("extra mutex", twoLocks, {2, 0, 0});
  succeeded &= check(twoLocks.hasViolations(1),
                     "An extra mutex escaped the callback budget");
  succeeded &=
      checkCounts("null free", measure([] { std::free(nullptr); }), {});
  const auto disabled = endAudit();
  auto *const pointer = std::malloc(128);
  std::free(pointer);
  lockAndUnlock();
  succeeded &=
      checkCounts("disabled interval", endAudit(), disabled);
  succeeded &= checkCounts("fresh interval", measure([] {}), {});
  return succeeded;
}

bool checkThreadIsolation() {
  std::atomic<int> ready{}, done{};
  std::atomic<bool> go{};
  std::array<Vst3RealtimeCounts, 2> results{};
  std::array<std::thread, 2> threads;
  for (std::size_t index = 0; index < threads.size(); ++index)
    threads[index] = std::thread([&, index] {
      onda_vst3_audit_begin();
      ready.fetch_add(1);
      while (!go.load())
        std::this_thread::yield();
      for (std::size_t i = 0; i <= index; ++i) {
        auto *const pointer = std::malloc(128);
        std::free(pointer);
      }
      onda_vst3_audit_end(&results[index]);
      done.fetch_add(1);
    });
  while (ready.load() != 2)
    std::this_thread::yield();
  const auto main = measure([&] {
    go.store(true);
    while (done.load() != 2)
      std::this_thread::yield();
  });
  for (auto &thread : threads)
    thread.join();
  return checkCounts("main thread", main, {}) &&
         checkCounts("first audio thread", results[0], {0, 1, 1}) &&
         checkCounts("second audio thread", results[1], {0, 2, 2});
}
} // namespace

int main(const int argc, const char *const *argv) {
  if (argc != 2)
    return 2;
  // Loading the real plugin adds TLS modules. The loader may free its old
  // thread-local storage while entering our allocation interceptors.
  auto *const module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (module == nullptr) {
    std::fprintf(stderr, "Could not load the VST3 module: %s\n", dlerror());
    return 1;
  }
  bool succeeded = checkAllocations();
  succeeded &= checkAllocationSemantics();
  succeeded &= checkIntervals();
  succeeded &= checkThreadIsolation();
  succeeded &= check(dlclose(module) == 0, "VST3 module unload failed");
  return succeeded ? 0 : 1;
}
