#pragma once

#include <chrono>
#include <thread>

namespace test {

inline constexpr auto waitTimeout = std::chrono::seconds(5);

// The caller chooses how progress happens: pumping a message loop and sleeping
// are deliberately separate, since some tests must prevent message-thread work.
template <typename Predicate, typename Progress>
bool waitUntil(Predicate predicate, Progress progress,
               std::chrono::milliseconds timeout = waitTimeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    progress();
  }
  return true;
}

inline void yieldToWorker() {
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

} // namespace test
