#include "FilesystemWatcher.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#error Unsupported filesystem watcher platform
#endif

namespace onda::plugin {
namespace {

using Path = std::filesystem::path;
using Paths = FilesystemWatcher::Paths;
using RootIdentity = std::array<std::uint64_t, 3>;

bool pathsOverlap(const Path &left, const Path &right) {
  const auto componentsEqual = [](const Path &a, const Path &b) {
#if defined(_WIN32)
    const auto leftText = a.native();
    const auto rightText = b.native();
    return CompareStringOrdinal(
               leftText.data(), static_cast<int>(leftText.size()),
               rightText.data(), static_cast<int>(rightText.size()),
               TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
  };
  const auto isWithin = [&componentsEqual](const Path &path, const Path &root) {
    auto pathPart = path.begin();
    for (auto rootPart = root.begin(); rootPart != root.end();
         ++rootPart, ++pathPart) {
      if (pathPart == path.end() || !componentsEqual(*pathPart, *rootPart))
        return false;
    }
    return true;
  };
  return isWithin(left, right) || isWithin(right, left);
}

void normalize(Paths &paths) {
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}

Path nearestExistingDirectory(const Path &path) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error)
    absolute = path;
  auto current = absolute.parent_path();
  while (!current.empty()) {
    error.clear();
    if (std::filesystem::is_directory(current, error) && !error)
      return current.lexically_normal();
    const auto parent = current.parent_path();
    if (parent == current)
      break;
    current = parent;
  }
  return {};
}

Path absoluteNormalized(const Path &path) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal();
}

struct PathProjection {
  Paths paths;
  Paths linkParents;
  bool complete{};
};

PathProjection projectLinkedPaths(Path path) {
  PathProjection result;
  // Bound traversal even for cyclic or expanding symlink chains. Incomplete
  // projections retain native coverage and also receive fallback polling.
  for (int followed = 0; followed < 64; ++followed) {
    if (std::ranges::find(result.paths, path) != result.paths.end())
      break;
    result.paths.push_back(path);
    Path prefix;
    auto linked = false;
    for (auto part = path.begin(); part != path.end(); ++part) {
      prefix /= *part;
      if (!prefix.is_absolute() || prefix == path.root_path())
        continue;
      std::error_code error;
      const auto status = std::filesystem::symlink_status(prefix, error);
      if (error && error != std::errc::no_such_file_or_directory &&
          error != std::errc::not_a_directory)
        return result;
      if (!std::filesystem::is_symlink(status))
        continue;
      result.linkParents.push_back(prefix.parent_path());
      auto target = std::filesystem::read_symlink(prefix, error);
      if (error)
        return result;
      if (target.is_relative())
        target = prefix.parent_path() / target;
      for (auto remaining = std::next(part); remaining != path.end();
           ++remaining)
        target /= *remaining;
      path = absoluteNormalized(target);
      linked = true;
      break;
    }
    if (!linked) {
      result.complete = true;
      break;
    }
  }
  normalize(result.paths);
  return result;
}

struct WatchPlan {
  Paths paths;
  std::vector<Paths> comparisonPaths;
  Paths roots;
  Paths contentRoots;
  std::vector<Paths> requirements;
  std::vector<std::optional<RootIdentity>> identities;

  [[nodiscard]] bool watchesContents(const Path &root) const noexcept {
    return std::binary_search(contentRoots.begin(), contentRoots.end(), root);
  }
};

std::optional<RootIdentity> rootIdentity(const Path &path) {
#if defined(_WIN32)
  const auto handle = CreateFileW(
      path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return std::nullopt;
  BY_HANDLE_FILE_INFORMATION info{};
  const auto success = GetFileInformationByHandle(handle, &info);
  CloseHandle(handle);
  if (success == FALSE)
    return std::nullopt;
  return RootIdentity{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
#else
  struct stat info{};
  if (stat(path.c_str(), &info) != 0)
    return std::nullopt;
  return RootIdentity{static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino), 0};
#endif
}

WatchPlan makeWatchPlan(Paths paths) {
  normalize(paths);
  WatchPlan plan{.paths = std::move(paths),
                 .comparisonPaths = {},
                 .roots = {},
                 .contentRoots = {},
                 .requirements = {},
                 .identities = {}};
  plan.comparisonPaths.reserve(plan.paths.size());
  plan.requirements.reserve(plan.paths.size());
  for (const auto &path : plan.paths) {
    auto projection = projectLinkedPaths(absoluteNormalized(path));
    plan.contentRoots.insert(plan.contentRoots.end(),
                             projection.linkParents.begin(),
                             projection.linkParents.end());
    Paths required;
    for (const auto &comparisonPath : projection.paths) {
      if (auto parent = nearestExistingDirectory(comparisonPath);
          !parent.empty()) {
        plan.contentRoots.push_back(parent);
        // Moves above the immediate parent leave its inode watch intact.
        for (;;) {
          required.push_back(parent);
          auto ancestor = parent.parent_path();
          if (ancestor == parent)
            break;
          parent = std::move(ancestor);
        }
      }
#if defined(__APPLE__)
      std::error_code error;
      if (std::filesystem::is_regular_file(comparisonPath, error) && !error) {
        required.push_back(comparisonPath);
        plan.contentRoots.push_back(comparisonPath);
      }
#endif
    }
    normalize(required);
    plan.roots.insert(plan.roots.end(), required.begin(), required.end());
    if (!projection.complete)
      required.clear();
    plan.comparisonPaths.push_back(std::move(projection.paths));
    plan.requirements.push_back(std::move(required));
  }
  normalize(plan.roots);
  normalize(plan.contentRoots);
  for (const auto &root : plan.roots)
    plan.identities.push_back(rootIdentity(root));
  return plan;
}

using NativeCallback = std::function<void(Paths, bool)>;

#if defined(__linux__)

class NativeWatcher final {
public:
  NativeWatcher(const WatchPlan &plan, NativeCallback callback)
      : callback_(std::move(callback)),
        inotify_(inotify_init1(IN_CLOEXEC | IN_NONBLOCK)),
        stop_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    if (inotify_ < 0 || stop_ < 0)
      return;
    constexpr auto lifecycleMask = IN_DELETE_SELF | IN_MOVE_SELF;
    constexpr auto contentMask = IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE |
                                 IN_DELETE | IN_MODIFY | IN_MOVED_FROM |
                                 IN_MOVED_TO;
    for (const auto &root : plan.roots) {
      // Different linked paths can share an inode through directory symlinks.
      const auto mask = static_cast<std::uint32_t>(
          lifecycleMask | (plan.watchesContents(root) ? contentMask : 0) |
          IN_MASK_ADD);
      const auto descriptor = inotify_add_watch(inotify_, root.c_str(), mask);
      if (descriptor >= 0) {
        roots_[descriptor].push_back(root);
        registered_.push_back(root);
      }
    }
    if (!roots_.empty())
      thread_ = std::thread([this] { run(); });
  }

  ~NativeWatcher() {
    if (thread_.joinable()) {
      const std::uint64_t signal = 1;
      ssize_t written;
      do {
        written = write(stop_, &signal, sizeof(signal));
      } while (written < 0 && errno == EINTR);
      thread_.join();
    }
    if (stop_ >= 0)
      close(stop_);
    if (inotify_ >= 0)
      close(inotify_);
  }

  [[nodiscard]] const Paths &registeredRoots() const noexcept {
    return registered_;
  }

private:
  void run() noexcept {
    alignas(inotify_event) std::array<std::byte, 64U * 1024U> buffer{};
    const std::array<pollfd, 2> initial{{
        {.fd = inotify_, .events = POLLIN, .revents = 0},
        {.fd = stop_, .events = POLLIN, .revents = 0},
    }};
    auto descriptors = initial;
    for (;;) {
      const auto result = poll(descriptors.data(), descriptors.size(), -1);
      if (result < 0) {
        if (errno == EINTR)
          continue;
        callback_({}, true);
        return;
      }
      if ((descriptors[1].revents & POLLIN) != 0)
        return;
      if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        callback_({}, true);
        return;
      }
      if ((descriptors[0].revents & POLLIN) == 0)
        continue;

      Paths changed;
      auto rescan = false;
      for (;;) {
        const auto count = read(inotify_, buffer.data(), buffer.size());
        if (count < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          if (errno == EINTR)
            continue;
          rescan = true;
          break;
        }
        if (count == 0)
          break;
        std::size_t offset{};
        const auto bytes = static_cast<std::size_t>(count);
        while (offset + sizeof(inotify_event) <= bytes) {
          const auto *event =
              reinterpret_cast<const inotify_event *>(buffer.data() + offset);
          if ((event->mask & IN_Q_OVERFLOW) != 0) {
            rescan = true;
          } else if (const auto root = roots_.find(event->wd);
                     root != roots_.end()) {
            for (const auto &path : root->second) {
              changed.push_back(event->len == 0 ? path
                                                : path / Path{event->name});
            }
          }
          offset += sizeof(inotify_event) + event->len;
        }
      }
      if (rescan || !changed.empty())
        callback_(std::move(changed), rescan);
    }
  }

  NativeCallback callback_;
  int inotify_{-1};
  int stop_{-1};
  std::map<int, Paths> roots_;
  Paths registered_;
  std::thread thread_;
};

#elif defined(__APPLE__)

class NativeWatcher final {
public:
  NativeWatcher(const WatchPlan &plan, NativeCallback callback)
      : callback_(std::move(callback)), queue_(kqueue()) {
    if (queue_ < 0 || pipe(stopPipe_) != 0)
      return;
    setCloseOnExec(stopPipe_[0]);
    setCloseOnExec(stopPipe_[1]);
    struct kevent stopEvent{};
    EV_SET(&stopEvent, static_cast<uintptr_t>(stopPipe_[0]), EVFILT_READ,
           EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(queue_, &stopEvent, 1, nullptr, 0, nullptr) != 0)
      return;

    for (const auto &root : plan.roots) {
      RootWatch watch{.path = root, .entries = {}};
      std::error_code error;
      if (plan.watchesContents(root) &&
          std::filesystem::is_directory(root, error) && !error) {
        Paths children;
        for (const auto &paths : plan.comparisonPaths) {
          for (const auto &path : paths) {
            const auto relative = path.lexically_relative(root);
            if (!relative.empty() && *relative.begin() != ".." &&
                *relative.begin() != ".")
              children.push_back(root / *relative.begin());
          }
        }
        normalize(children);
        for (auto &child : children) {
          const auto identity = entryIdentity(child);
          watch.entries.push_back({std::move(child), identity});
        }
      }
      const auto descriptor = open(root.c_str(), O_EVTONLY | O_CLOEXEC);
      if (descriptor < 0)
        continue;
      struct kevent event{};
      const auto flags = static_cast<std::uint32_t>(
          NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE |
          (plan.watchesContents(root)
               ? NOTE_ATTRIB | NOTE_EXTEND | NOTE_LINK | NOTE_WRITE
               : 0));
      EV_SET(&event, static_cast<uintptr_t>(descriptor), EVFILT_VNODE,
             EV_ADD | EV_CLEAR, flags, 0, nullptr);
      if (kevent(queue_, &event, 1, nullptr, 0, nullptr) != 0) {
        close(descriptor);
        continue;
      }
      roots_.emplace(descriptor, std::move(watch));
      registered_.push_back(root);
    }
    if (!roots_.empty())
      thread_ = std::thread([this] { run(); });
  }

  ~NativeWatcher() {
    if (thread_.joinable()) {
      const char signal = 1;
      static_cast<void>(write(stopPipe_[1], &signal, sizeof(signal)));
      thread_.join();
    }
    for (const auto &[descriptor, path] : roots_) {
      static_cast<void>(path);
      close(descriptor);
    }
    if (stopPipe_[0] >= 0)
      close(stopPipe_[0]);
    if (stopPipe_[1] >= 0)
      close(stopPipe_[1]);
    if (queue_ >= 0)
      close(queue_);
  }

  [[nodiscard]] const Paths &registeredRoots() const noexcept {
    return registered_;
  }

private:
  struct EntryIdentity {
    dev_t device{};
    ino_t inode{};

    friend bool operator==(const EntryIdentity &,
                           const EntryIdentity &) = default;
  };

  struct DirectoryEntry {
    Path path;
    std::optional<EntryIdentity> identity;
  };

  struct RootWatch {
    Path path;
    std::vector<DirectoryEntry> entries;
  };

  static std::optional<EntryIdentity> entryIdentity(const Path &path) noexcept {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0)
      return std::nullopt;
    return EntryIdentity{info.st_dev, info.st_ino};
  }

  static void setCloseOnExec(const int descriptor) {
    const auto flags = fcntl(descriptor, F_GETFD);
    if (flags >= 0)
      static_cast<void>(fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC));
  }

  void run() noexcept {
    std::array<struct kevent, 32> events{};
    for (;;) {
      const auto count = kevent(queue_, nullptr, 0, events.data(),
                                static_cast<int>(events.size()), nullptr);
      if (count < 0) {
        if (errno == EINTR)
          continue;
        callback_({}, true);
        return;
      }
      Paths changed;
      for (int index = 0; index < count; ++index) {
        const auto descriptor =
            static_cast<int>(events[static_cast<std::size_t>(index)].ident);
        if (descriptor == stopPipe_[0])
          return;
        if (const auto root = roots_.find(descriptor); root != roots_.end()) {
          auto &watch = root->second;
          const auto lifecycle =
              events[static_cast<std::size_t>(index)].fflags &
              (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE);
          if (lifecycle != 0 || watch.entries.empty()) {
            changed.push_back(watch.path);
          } else {
            // kqueue names the directory, not the changed entry. Only report
            // replacement of a child on a watched path; file vnode watches
            // independently report in-place edits, even with unchanged
            // metadata.
            for (auto &entry : watch.entries) {
              const auto identity = entryIdentity(entry.path);
              if (identity != entry.identity) {
                entry.identity = identity;
                changed.push_back(entry.path);
              }
            }
          }
        }
      }
      if (!changed.empty())
        callback_(std::move(changed), false);
    }
  }

  NativeCallback callback_;
  int queue_{-1};
  int stopPipe_[2]{-1, -1};
  std::map<int, RootWatch> roots_;
  Paths registered_;
  std::thread thread_;
};

#elif defined(_WIN32)

class NativeWatcher final {
public:
  NativeWatcher(const WatchPlan &plan, NativeCallback callback)
      : callback_(std::move(callback)),
        completionPort_(
            CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)) {
    if (completionPort_ == nullptr)
      return;
    for (const auto &root : plan.roots) {
      auto entry = std::make_unique<Entry>();
      entry->root = root;
      entry->filter = FILE_NOTIFY_CHANGE_DIR_NAME;
      if (plan.watchesContents(root)) {
        entry->filter |= FILE_NOTIFY_CHANGE_FILE_NAME |
                         FILE_NOTIFY_CHANGE_ATTRIBUTES |
                         FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                         FILE_NOTIFY_CHANGE_CREATION;
      }
      entry->directory = CreateFileW(
          root.c_str(), FILE_LIST_DIRECTORY,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
          nullptr);
      if (entry->directory == INVALID_HANDLE_VALUE)
        continue;
      if (CreateIoCompletionPort(entry->directory, completionPort_,
                                 reinterpret_cast<ULONG_PTR>(entry.get()),
                                 0) == nullptr ||
          !issue(*entry)) {
        CloseHandle(entry->directory);
        continue;
      }
      registered_.push_back(root);
      entries_.push_back(std::move(entry));
    }
    if (!entries_.empty())
      thread_ = std::thread([this] { run(); });
  }

  ~NativeWatcher() {
    if (thread_.joinable()) {
      static_cast<void>(
          PostQueuedCompletionStatus(completionPort_, 0, stopKey, nullptr));
      thread_.join();
    }
    for (auto &entry : entries_)
      CloseHandle(entry->directory);
    if (completionPort_ != nullptr)
      CloseHandle(completionPort_);
  }

  [[nodiscard]] const Paths &registeredRoots() const noexcept {
    return registered_;
  }

private:
  struct Entry {
    Path root;
    HANDLE directory{INVALID_HANDLE_VALUE};
    OVERLAPPED operation{};
    DWORD filter{};
    alignas(
        FILE_NOTIFY_INFORMATION) std::array<std::byte, 32U * 1024U> buffer{};
    bool pending{};
  };

  static constexpr ULONG_PTR stopKey = 1;

  static bool issue(Entry &entry) {
    entry.operation = {};
    entry.pending =
        ReadDirectoryChangesW(
            entry.directory, entry.buffer.data(),
            static_cast<DWORD>(entry.buffer.size()), FALSE, entry.filter,
            nullptr, &entry.operation, nullptr) != FALSE;
    return entry.pending;
  }

  void cancelPending() noexcept {
    // The IOCP thread exclusively owns pending state. Drain every cancelled
    // completion before the entries and their OVERLAPPED storage are destroyed.
    auto remaining = std::size_t{};
    for (auto &entry : entries_) {
      if (!entry->pending)
        continue;
      ++remaining;
      static_cast<void>(CancelIoEx(entry->directory, &entry->operation));
    }

    while (remaining != 0U) {
      DWORD bytes{};
      ULONG_PTR key{};
      OVERLAPPED *operation{};
      static_cast<void>(GetQueuedCompletionStatus(
          completionPort_, &bytes, &key, &operation, INFINITE));
      auto *entry = reinterpret_cast<Entry *>(key);
      if (entry != nullptr && operation == &entry->operation && entry->pending) {
        entry->pending = false;
        --remaining;
      }
    }
  }

  void run() noexcept {
    for (;;) {
      DWORD bytes{};
      ULONG_PTR key{};
      OVERLAPPED *operation{};
      const auto success = GetQueuedCompletionStatus(
          completionPort_, &bytes, &key, &operation, INFINITE);
      if (key == stopKey) {
        cancelPending();
        return;
      }
      auto *entry = reinterpret_cast<Entry *>(key);
      if (entry == nullptr || operation != &entry->operation) {
        callback_({}, true);
        return;
      }
      entry->pending = false;
      Paths changed;
      auto rescan = success == FALSE || bytes == 0;
      if (!rescan) {
        DWORD offset{};
        for (;;) {
          const auto *information =
              reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(
                  entry->buffer.data() + offset);
          changed.push_back(entry->root /
                            Path{std::wstring{information->FileName,
                                              information->FileNameLength /
                                                  sizeof(wchar_t)}});
          if (information->NextEntryOffset == 0)
            break;
          offset += information->NextEntryOffset;
        }
      }
      if (!issue(*entry))
        rescan = true;
      callback_(std::move(changed), rescan);
    }
  }

  NativeCallback callback_;
  HANDLE completionPort_{};
  std::vector<std::unique_ptr<Entry>> entries_;
  Paths registered_;
  std::thread thread_;
};

#endif

} // namespace

struct FilesystemWatcher::SharedState {
  explicit SharedState(Callback changed) : callback(std::move(changed)) {}

  Callback callback;
  std::atomic<std::uint64_t> revision{};
};

class FilesystemWatcher::Impl final {
public:
  Impl(WatchPlan plan, std::shared_ptr<SharedState> state)
      : plan_(std::move(plan)), state_(std::move(state)) {
    refreshCoverage();
  }

  [[nodiscard]] bool matches(const WatchPlan &plan) const {
    return plan_.paths == plan.paths &&
           plan_.comparisonPaths == plan.comparisonPaths &&
           plan_.roots == plan.roots &&
           plan_.contentRoots == plan.contentRoots &&
           plan_.identities == plan.identities;
  }

  void refresh(std::vector<Paths> requirements) {
    // Requirements may become complete after permissions recover. They are
    // worker-owned; the native callbacks only read paths and projections.
    plan_.requirements = std::move(requirements);
    refreshCoverage();
  }

  [[nodiscard]] const Paths &fallbackPaths() const noexcept {
    return fallback_;
  }

private:
  void refreshCoverage() {
    if (rescan_.exchange(false, std::memory_order_acq_rel)) {
      native_.clear();
      registered_.clear();
    }
    Paths missingRoots;
    std::set_difference(plan_.roots.begin(), plan_.roots.end(), registered_.begin(),
                        registered_.end(), std::back_inserter(missingRoots));
    if (!missingRoots.empty()) {
      auto missing = plan_;
      missing.roots = std::move(missingRoots);
      auto native = std::make_unique<NativeWatcher>(
          missing, [this](Paths paths, const bool rescan) {
            observe(std::move(paths), rescan);
          });
      const auto &added = native->registeredRoots();
      if (!added.empty()) {
        registered_.insert(registered_.end(), added.begin(), added.end());
        normalize(registered_);
        native_.push_back(std::move(native));
      }
    }
    fallback_.clear();
    for (std::size_t index = 0; index < plan_.paths.size(); ++index) {
      const auto &required = plan_.requirements[index];
      if (required.empty() ||
          std::any_of(required.begin(), required.end(), [&](const Path &root) {
            return !std::binary_search(registered_.begin(), registered_.end(),
                                       root);
          })) {
        fallback_.push_back(plan_.paths[index]);
      }
    }
  }

  void observe(Paths nativePaths, const bool rescan) noexcept {
    try {
      if (rescan)
        rescan_.store(true, std::memory_order_release);
      Paths affected;
      for (std::size_t index = 0; index < plan_.paths.size(); ++index) {
        const auto &projections = plan_.comparisonPaths[index];
        if (rescan || std::any_of(nativePaths.begin(), nativePaths.end(),
                                  [&](const Path &changed) {
                                    return std::ranges::any_of(
                                        projections, [&](const Path &watched) {
                                          return pathsOverlap(watched, changed);
                                        });
                                  })) {
          affected.push_back(plan_.paths[index]);
        }
      }
      if (affected.empty())
        return;
      state_->revision.fetch_add(1, std::memory_order_release);
      normalize(affected);
      state_->callback(std::move(affected));
    } catch (...) {
    }
  }

  WatchPlan plan_;
  std::shared_ptr<SharedState> state_;
  std::atomic<bool> rescan_{};
  Paths registered_;
  Paths fallback_;
  // Recovery subscriptions cover only roots that failed previously. Keeping
  // successful groups alive avoids rebuilding healthy OS watches every poll.
  std::vector<std::unique_ptr<NativeWatcher>> native_;
};

FilesystemWatcher::FilesystemWatcher(Callback callback)
    : state_(std::make_shared<SharedState>(std::move(callback))) {}

FilesystemWatcher::~FilesystemWatcher() = default;

FilesystemWatcher::Paths
FilesystemWatcher::watch(const std::span<const Path> paths) {
  auto plan = makeWatchPlan(Paths{paths.begin(), paths.end()});
  if (impl_ && impl_->matches(plan)) {
    impl_->refresh(std::move(plan.requirements));
    return impl_->fallbackPaths();
  }
  auto next = paths.empty() ? std::unique_ptr<Impl>{}
                            : std::make_unique<Impl>(std::move(plan), state_);
  auto fallback = next ? next->fallbackPaths() : Paths{};
  impl_.swap(next);
  return fallback;
}

std::uint64_t FilesystemWatcher::revision() const noexcept {
  return state_->revision.load(std::memory_order_acquire);
}

} // namespace onda::plugin
