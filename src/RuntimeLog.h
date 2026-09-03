#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace onda::plugin {

enum class RuntimeLogKind : std::uint8_t {
  print,
  delegate,
};

inline constexpr std::size_t runtimeLogTextCapacity = 4096U;
inline constexpr std::size_t runtimeLogPathCapacity = 1024U;
inline constexpr std::size_t runtimeLogOwnerCapacity = 256U;
inline constexpr std::size_t runtimeLogQueueCapacity = 256U;

struct RuntimeLogEntry {
  RuntimeLogKind kind{};
  std::uint64_t generation{};
  std::uint64_t epoch{};
  std::array<char, runtimeLogTextCapacity> text{};
  std::uint32_t textBytes{};
  std::array<char, runtimeLogPathCapacity> sourceFile{};
  std::uint32_t sourceFileBytes{};
  std::array<char, runtimeLogOwnerCapacity> lexicalOwner{};
  std::uint32_t lexicalOwnerBytes{};
  std::uint32_t line{};

  void prepare(const RuntimeLogKind nextKind,
               const std::uint64_t nextGeneration) noexcept {
    kind = nextKind;
    generation = nextGeneration;
    textBytes = 0U;
    sourceFileBytes = 0U;
    lexicalOwnerBytes = 0U;
    line = 0U;
  }
};

struct RuntimeLogCounters {
  std::uint64_t printOverflow{};
  std::uint64_t printTransportDrops{};
  std::uint64_t delegateOverflow{};
  std::uint64_t delegateTransportDrops{};
};

inline void addSaturated(std::uint64_t &target,
                         const std::uint64_t value) noexcept {
  target = value > std::numeric_limits<std::uint64_t>::max() - target
               ? std::numeric_limits<std::uint64_t>::max()
               : target + value;
}

class RuntimeLogSink final {
public:
  template <typename Writer>
  [[nodiscard]] bool tryEmplace(const RuntimeLogKind kind,
                                const std::uint64_t generation,
                                Writer &&writer) noexcept {
    const auto write = write_.load(std::memory_order_relaxed);
    const auto next = increment(write);
    if (next == read_.load(std::memory_order_acquire))
      return false;

    auto &entry = entries_[write];
    entry.prepare(kind, generation);
    entry.epoch = epoch_.load(std::memory_order_acquire);
    if (!std::forward<Writer>(writer)(entry))
      return false;
    write_.store(next, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool tryPush(const RuntimeLogEntry &entry) noexcept {
    return tryPush(entry, entry.generation);
  }

  [[nodiscard]] bool tryPush(const RuntimeLogEntry &entry,
                             const std::uint64_t generation) noexcept {
    if (entry.textBytes > entry.text.size() ||
        entry.sourceFileBytes > entry.sourceFile.size() ||
        entry.lexicalOwnerBytes > entry.lexicalOwner.size()) {
      return false;
    }
    return tryEmplace(
        entry.kind, generation,
        [&entry](RuntimeLogEntry &destination) noexcept {
          std::memcpy(destination.text.data(), entry.text.data(),
                      entry.textBytes);
          destination.textBytes = entry.textBytes;
          std::memcpy(destination.sourceFile.data(), entry.sourceFile.data(),
                      entry.sourceFileBytes);
          destination.sourceFileBytes = entry.sourceFileBytes;
          std::memcpy(destination.lexicalOwner.data(),
                      entry.lexicalOwner.data(), entry.lexicalOwnerBytes);
          destination.lexicalOwnerBytes = entry.lexicalOwnerBytes;
          destination.line = entry.line;
          return true;
        });
  }

  [[nodiscard]] bool tryPop(RuntimeLogEntry &entry) noexcept {
    const auto read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire))
      return false;
    entry = entries_[read];
    read_.store(increment(read), std::memory_order_release);
    return true;
  }

  void addGeneratedOverflow(const RuntimeLogKind kind,
                            const std::uint64_t count) noexcept {
    if (count != 0U)
      addEpochCounter(generatedOverflow(kind), count);
  }

  void addTransportDrops(const RuntimeLogKind kind,
                         const std::uint64_t count = 1U) noexcept {
    if (count != 0U)
      addEpochCounter(transportDrops(kind), count);
  }

  [[nodiscard]] RuntimeLogCounters
  counterTotals(const std::uint64_t epoch) const noexcept {
    return {
        .printOverflow = counterTotal(printOverflow_, epoch),
        .printTransportDrops = counterTotal(printTransportDrops_, epoch),
        .delegateOverflow = counterTotal(delegateOverflow_, epoch),
        .delegateTransportDrops = counterTotal(delegateTransportDrops_, epoch),
    };
  }

  [[nodiscard]] std::uint64_t beginEpoch() noexcept {
    return epoch_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
  }

  [[nodiscard]] std::uint64_t epoch() const noexcept {
    return epoch_.load(std::memory_order_acquire);
  }

private:
  struct EpochCounter {
    std::atomic<std::uint64_t> sequence{};
    std::atomic<std::uint64_t> epoch{};
    std::atomic<std::uint64_t> value{};
  };

  static void addAtomicSaturated(std::atomic<std::uint64_t> &target,
                                 const std::uint64_t value) noexcept {
    auto current = target.load(std::memory_order_relaxed);
    for (;;) {
      const auto next =
          value > std::numeric_limits<std::uint64_t>::max() - current
              ? std::numeric_limits<std::uint64_t>::max()
              : current + value;
      if (target.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
        return;
      }
    }
  }

  void addEpochCounter(EpochCounter &counter,
                       const std::uint64_t value) noexcept {
    const auto currentEpoch = epoch_.load(std::memory_order_acquire);
    if (counter.epoch.load(std::memory_order_seq_cst) != currentEpoch) {
      counter.sequence.fetch_add(1U, std::memory_order_seq_cst);
      counter.value.store(0U, std::memory_order_seq_cst);
      counter.epoch.store(currentEpoch, std::memory_order_seq_cst);
      counter.sequence.fetch_add(1U, std::memory_order_seq_cst);
    }
    addAtomicSaturated(counter.value, value);
  }

  [[nodiscard]] static std::uint64_t
  counterTotal(const EpochCounter &counter,
               const std::uint64_t epoch) noexcept {
    for (;;) {
      const auto before = counter.sequence.load(std::memory_order_seq_cst);
      if ((before & 1U) != 0U)
        continue;
      const auto counterEpoch = counter.epoch.load(std::memory_order_seq_cst);
      const auto value = counter.value.load(std::memory_order_seq_cst);
      const auto after = counter.sequence.load(std::memory_order_seq_cst);
      if (before == after)
        return counterEpoch == epoch ? value : 0U;
    }
  }

  [[nodiscard]] static constexpr std::size_t
  increment(const std::size_t index) noexcept {
    return (index + 1U) % runtimeLogQueueCapacity;
  }

  [[nodiscard]] EpochCounter &
  generatedOverflow(const RuntimeLogKind kind) noexcept {
    return kind == RuntimeLogKind::print ? printOverflow_ : delegateOverflow_;
  }

  [[nodiscard]] EpochCounter &
  transportDrops(const RuntimeLogKind kind) noexcept {
    return kind == RuntimeLogKind::print ? printTransportDrops_
                                         : delegateTransportDrops_;
  }

  std::array<RuntimeLogEntry, runtimeLogQueueCapacity> entries_{};
  std::atomic<std::size_t> read_{};
  std::atomic<std::size_t> write_{};
  std::atomic<std::uint64_t> epoch_{1U};
  EpochCounter printOverflow_;
  EpochCounter printTransportDrops_;
  EpochCounter delegateOverflow_;
  EpochCounter delegateTransportDrops_;
};

struct RuntimeLogRecord {
  RuntimeLogKind kind{};
  std::string text;
  std::string sourceFile;
  std::string lexicalOwner;
  std::uint32_t line{};
};

struct RuntimeLogSnapshot {
  std::vector<RuntimeLogRecord> records;
  RuntimeLogCounters counters;
  bool revealed{};
};

} // namespace onda::plugin
