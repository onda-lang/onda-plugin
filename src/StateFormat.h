#pragma once

#include <cstdint>

namespace onda::plugin {

inline constexpr std::uint32_t stateMagic = 0x41444e4fU; // ONDA, little-endian.
inline constexpr int stateVersion = 5;

} // namespace onda::plugin
