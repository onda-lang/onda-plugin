#include "AllocationAuditTestSupport.h"
#include "RealtimeAudit.h"

#include <cerrno>
#include <cstdio>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace test {
bool exerciseRealtimeAuditInstrumentation() {
  bool succeeded = true;
  for (const auto &test : allocationAuditCases()) {
    void *pointer{};
    const auto missedAllocation =
        auditRealtime([&] { pointer = test.allocate(); });
    if (missedAllocation || pointer == nullptr) {
      std::fprintf(stderr, "Processor audit missed %s allocation\n", test.name);
      succeeded = false;
    }
    const auto missedRelease = auditRealtime([&] { test.release(pointer); });
    if (missedRelease) {
      std::fprintf(stderr, "Processor audit missed %s deallocation\n",
                   test.name);
      succeeded = false;
    }
  }
  if (!auditRealtime([] {})) {
    std::fputs("Processor audit did not reset its counters\n", stderr);
    succeeded = false;
  }
#if defined(__linux__)
  pthread_mutex_t mutex{};
  if (pthread_mutex_init(&mutex, nullptr) != 0) {
    std::fputs("Processor audit mutex initialization failed\n", stderr);
    return false;
  }
  int result{};
  const auto missedLock =
      auditRealtime([&] { result = pthread_mutex_lock(&mutex); });
  if (missedLock || result != 0) {
    std::fputs("Processor audit missed a mutex lock\n", stderr);
    succeeded = false;
  }
  if (result == 0) {
    if (pthread_mutex_trylock(&mutex) != EBUSY) {
      std::fputs("Processor audit did not forward the mutex lock\n", stderr);
      succeeded = false;
    }
    pthread_mutex_unlock(&mutex);
  }
  pthread_mutex_destroy(&mutex);
  if (!auditRealtime([] { std::free(nullptr); })) {
    std::fputs("Processor audit counted a null free\n", stderr);
    succeeded = false;
  }
#endif
  return succeeded;
}
} // namespace test
