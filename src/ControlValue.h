#pragma once

#include "Product.h"
#include "ScalarValue.h"

#include <array>

namespace onda::plugin {

inline constexpr std::size_t controlCount =
    static_cast<std::size_t>(pluginControlInputs);

using ControlValue = ScalarValue;

using ControlValues = std::array<ControlValue, controlCount>;

} // namespace onda::plugin
