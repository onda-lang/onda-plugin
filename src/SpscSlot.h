#pragma once

#include <atomic>
#include <utility>

namespace onda::plugin {

template <typename Item> class SpscSlot final {
  static_assert(std::is_pointer_v<Item>);

public:
  [[nodiscard]] bool tryPush(Item item) noexcept {
    Item expected{};
    return value_.compare_exchange_strong(
        expected, item, std::memory_order_release, std::memory_order_relaxed);
  }

  [[nodiscard]] Item tryPop() noexcept {
    return value_.exchange(Item{}, std::memory_order_acquire);
  }

  [[nodiscard]] bool empty() const noexcept {
    return value_.load(std::memory_order_acquire) == Item{};
  }

private:
  std::atomic<Item> value_{};
};

} // namespace onda::plugin
