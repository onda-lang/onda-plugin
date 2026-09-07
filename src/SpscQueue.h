#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

namespace onda::plugin {

template <typename Item, std::size_t Capacity> class SpscQueue final {
  static_assert(Capacity > 0U);

public:
  [[nodiscard]] bool tryPush(const Item &item) noexcept {
    const auto write = write_.load(std::memory_order_relaxed);
    const auto next = increment(write);
    if (next == read_.load(std::memory_order_acquire))
      return false;
    items_[write] = item;
    write_.store(next, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool tryPop(Item &item) noexcept {
    const auto read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire))
      return false;
    item = items_[read];
    read_.store(increment(read), std::memory_order_release);
    return true;
  }

private:
  static constexpr auto storageSize = Capacity + 1U;
  [[nodiscard]] static constexpr std::size_t
  increment(const std::size_t value) noexcept {
    return value + 1U == storageSize ? 0U : value + 1U;
  }

  std::unique_ptr<Item[]> items_{std::make_unique<Item[]>(storageSize)};
  std::atomic<std::size_t> write_{};
  std::atomic<std::size_t> read_{};
};

} // namespace onda::plugin
