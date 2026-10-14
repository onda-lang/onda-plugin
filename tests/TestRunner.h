#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>

namespace test {

struct Case {
  std::string_view name;
  bool (*run)();
};

inline void reportException() noexcept {
  const auto error = std::current_exception();
  if (!error) {
    std::fputs("No active exception\n", stderr);
    return;
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::exception &exception) {
    std::fprintf(stderr, "%s\n", exception.what());
  } catch (...) {
    std::fputs("Unknown exception\n", stderr);
  }
}

inline int runCases(const int argc, const char *const *argv,
                    const std::span<const Case> cases) {
  if (argc > 2) {
    std::cerr << "usage: " << argv[0] << " [case-name]\n";
    return 2;
  }
  // Uncaught worker-thread and noexcept exceptions bypass the case's catch.
  std::set_terminate([] {
    std::fputs("Test terminated outside the case exception handler\n", stderr);
    reportException();
    std::fflush(stderr);
    std::abort();
  });
  bool selected{};
  bool succeeded = true;
  for (const auto &testCase : cases) {
    if (argc == 2 && testCase.name != argv[1])
      continue;
    selected = true;
    // Flush before entering native code, including code that can abort.
    std::cout << "[ RUN      ] " << testCase.name << std::endl;
    bool passed{};
    try {
      passed = testCase.run();
    } catch (...) {
      reportException();
    }
    std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ")
              << testCase.name << std::endl;
    succeeded &= passed;
  }
  if (!selected) {
    std::cerr << "unknown test case\n";
    return 2;
  }
  return succeeded ? 0 : 1;
}

} // namespace test
