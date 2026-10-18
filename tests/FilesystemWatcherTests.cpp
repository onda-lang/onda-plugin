#include "FilesystemWatcher.h"
#include "TestRunner.h"
#include "TestWait.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>

namespace {
using onda::plugin::FilesystemWatcher;
namespace fs = std::filesystem;

struct Fixture {
  fs::path root =
      fs::canonical(fs::temp_directory_path()) /
      ("onda-native-watch-" + std::to_string(std::random_device{}()));

  Fixture() {
    if (!fs::create_directory(root))
      throw std::runtime_error("could not create watcher fixture");
  }
  ~Fixture() {
    std::error_code ignored;
    fs::permissions(root / "blocked", fs::perms::owner_all, ignored);
    fs::remove_all(root, ignored);
  }
};

void write(const fs::path &path, const char *text) {
  std::ofstream stream(path);
  stream << text;
  if (!stream.good())
    throw std::runtime_error("could not write watcher fixture");
}

bool topologyRecovery() {
  Fixture fixture;
  const auto directory = fixture.root / "code";
  const auto source = directory / "main.onda";
  const std::array paths{source};
  std::atomic<unsigned> changes{};
  FilesystemWatcher watcher([&](const auto &affected) {
    if (std::ranges::find(affected, source) != affected.end())
      changes.fetch_add(1, std::memory_order_release);
  });
  if (!watcher.watch(paths).empty())
    return false;
  fs::create_directory(directory);
  write(source, "first");
  if (!test::waitUntil(
          [&] { return changes.load(std::memory_order_acquire) > 0; },
          test::yieldToWorker))
    return false;
  if (!watcher.watch(paths).empty())
    return false;
  for (const auto *text : {"second", "third"}) {
    const auto revision = watcher.revision();
    write(source, text);
    if (!test::waitUntil([&] { return watcher.revision() > revision; },
                         test::yieldToWorker))
      return false;
  }
#if !defined(_WIN32)
  fs::rename(directory, fixture.root / "previous");
  fs::create_directory(directory);
  write(source, "replacement");
  if (!watcher.watch(paths).empty())
    return false;
  const auto revision = watcher.revision();
  write(source, "edited after replacement");
  if (!test::waitUntil([&] { return watcher.revision() > revision; },
                       test::yieldToWorker))
    return false;
#endif
  return true;
}

#if defined(__linux__)
std::set<fs::path> inotifyDescriptors() {
  std::set<fs::path> result;
  for (const auto &entry : fs::directory_iterator("/proc/self/fd")) {
    std::error_code error;
    const auto target = fs::read_symlink(entry.path(), error);
    if (!error && target == "anon_inode:inotify")
      result.insert(entry.path());
  }
  return result;
}

bool subscriptionReuse() {
  Fixture fixture;
  const auto healthy = fixture.root / "healthy.onda";
  const auto blocked = fixture.root / "blocked";
  const auto missingCoverage = blocked / "main.onda";
  fs::create_directory(blocked);
  write(healthy, "healthy");
  write(missingCoverage, "blocked");
  fs::permissions(blocked, fs::perms::none);
  std::atomic<bool> observed{};
  FilesystemWatcher watcher([&](const auto &paths) {
    if (std::ranges::find(paths, missingCoverage) != paths.end())
      observed.store(true, std::memory_order_release);
  });
  const std::array paths{healthy, missingCoverage};
  if (watcher.watch(paths).empty()) {
    std::cerr << "permission failure did not produce fallback coverage\n";
    return false;
  }
  const auto original = inotifyDescriptors();
  for (int retry = 0; retry < 3; ++retry) {
    if (watcher.watch(paths).empty() || inotifyDescriptors() != original) {
      std::cerr << "fallback retry replaced healthy subscriptions\n";
      return false;
    }
  }
  fs::permissions(blocked, fs::perms::owner_all);
  if (!watcher.watch(paths).empty())
    return false;
  const auto recovered = inotifyDescriptors();
  if (!std::includes(recovered.begin(), recovered.end(), original.begin(),
                     original.end()))
    return false;
  if (!watcher.watch(paths).empty() || inotifyDescriptors() != recovered)
    return false;
  observed.store(false, std::memory_order_release);
  write(missingCoverage, "native watching restored");
  return test::waitUntil(
      [&] { return observed.load(std::memory_order_acquire); },
      test::yieldToWorker);
}
#endif
} // namespace

int main(int argc, char **argv) {
  const std::array cases{
      test::Case{"WatcherTopologyRecovery", topologyRecovery, "native"},
#if defined(__linux__)
      test::Case{"WatcherSubscriptionReuse", subscriptionReuse, "native"},
#endif
  };
  return test::runCases(argc, argv, cases);
}
