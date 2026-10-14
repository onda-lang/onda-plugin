#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <variant>

namespace onda::plugin {

using ScalarPlain = std::variant<double, std::int64_t>;

inline double scalarPlainNumber(const ScalarPlain &value) noexcept {
  return std::visit(
      [](const auto scalar) { return static_cast<double>(scalar); }, value);
}

inline std::uint64_t scalarPlainBits(const ScalarPlain &value) noexcept {
  return std::visit(
      [](const auto scalar) { return std::bit_cast<std::uint64_t>(scalar); },
      value);
}

inline ScalarPlain scalarPlainFromBits(const std::uint64_t bits,
                                       const bool integer) noexcept {
  if (integer)
    return std::bit_cast<std::int64_t>(bits);
  return std::bit_cast<double>(bits);
}

// Only the numeric domain determines whether an exact plain default survives
// recompilation. Names, units and a declaration's new default do not.
struct ScalarDomain {
  enum class Type : std::uint8_t { f32, f64, i32, i64, boolean };
  double minimum{}, maximum{};
  double step{}, curve{};
  std::uint32_t stepCount{};
  Type type{Type::f32};
  bool logarithmic{}, hasCurve{};
  // An implicit UI range must not constrain an unranged Onda default.
  bool defaultMayExceedRange{};

  friend bool operator==(const ScalarDomain &, const ScalarDomain &) = default;
};

// Normalization cannot preserve every plain default, even in double precision.
// Host/editor writes carry only a normalized value and replace this identity.
struct ScalarValue {
  struct ScalarDefault {
    ScalarPlain plain{};
    ScalarDomain domain;
  };

  ScalarValue(double value = 0.5) noexcept
      : normalized(std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.5) {}

  std::optional<ScalarPlain>
  defaultPlain(const ScalarDomain &domain) const noexcept {
    return scalarDefault && scalarDefault->domain == domain
               ? std::optional{scalarDefault->plain}
               : std::nullopt;
  }

  float plain(double minimum, double maximum) const noexcept {
    if (const auto exact = defaultPlain({minimum, maximum}))
      return static_cast<float>(scalarPlainNumber(*exact));
    return static_cast<float>(std::lerp(minimum, maximum, normalized));
  }

  double normalized;
  std::optional<ScalarDefault> scalarDefault;
};

} // namespace onda::plugin
