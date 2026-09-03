#pragma once

#include <cstdint>
#include <string_view>

namespace onda::plugin {

#ifndef ONDA_PLUGIN_INPUT_CHANNELS
#error "ONDA_PLUGIN_INPUT_CHANNELS must be defined by the build system"
#endif

#ifndef ONDA_PLUGIN_OUTPUT_CHANNELS
#error "ONDA_PLUGIN_OUTPUT_CHANNELS must be defined by the build system"
#endif

inline constexpr int pluginInputChannels = ONDA_PLUGIN_INPUT_CHANNELS;
inline constexpr int pluginOutputChannels = ONDA_PLUGIN_OUTPUT_CHANNELS;
inline constexpr int pluginPassthroughChannels =
    pluginInputChannels < pluginOutputChannels ? pluginInputChannels
                                               : pluginOutputChannels;
static_assert(pluginInputChannels >= 0 && pluginInputChannels <= 64);
static_assert(pluginOutputChannels >= 1 && pluginOutputChannels <= 64);

enum class Product : std::uint8_t {
  instrument,
  effect,
};

[[nodiscard]] constexpr std::string_view productName(Product product) noexcept {
  return product == Product::instrument ? "OndaSynth" : "OndaFX";
}

} // namespace onda::plugin
