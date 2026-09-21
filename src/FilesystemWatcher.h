#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace onda::plugin {

// Native notifications are change hints. Callers remain responsible for
// validating the contents of the returned paths before acting on them.
class FilesystemWatcher final {
public:
  using Paths = std::vector<std::filesystem::path>;
  // Called on the native watcher thread with the exact watched paths affected
  // by a platform event. The callback should only enqueue work.
  using Callback = std::function<void(Paths)>;

  explicit FilesystemWatcher(Callback callback);
  ~FilesystemWatcher();

  FilesystemWatcher(const FilesystemWatcher &) = delete;
  FilesystemWatcher &operator=(const FilesystemWatcher &) = delete;

  // Replaces the watch set and returns exact paths not fully covered by the
  // native backend. Existing subscriptions stay live until replacements have
  // been installed.
  [[nodiscard]] Paths watch(std::span<const std::filesystem::path> paths);
  [[nodiscard]] std::uint64_t revision() const noexcept;

private:
  struct SharedState;
  class Impl;

  std::shared_ptr<SharedState> state_;
  std::unique_ptr<Impl> impl_;
};

} // namespace onda::plugin
