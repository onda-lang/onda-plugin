#pragma once

#include <cstdint>
#include <string_view>

namespace onda::plugin {

enum class Product : std::uint8_t {
  instrument,
  effect,
};

[[nodiscard]] constexpr std::string_view productName(Product product) noexcept {
  return product == Product::instrument ? "OndaSynth" : "OndaFX";
}

[[nodiscard]] constexpr int hostInputChannels(Product product) noexcept {
  return product == Product::instrument ? 0 : 2;
}

} // namespace onda::plugin
