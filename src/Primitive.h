#pragma once

#include <onda.h>

#include <cstddef>
#include <string_view>

namespace onda::plugin {

struct PrimitiveMetadata {
  std::string_view name;
  std::size_t bytes{};
};

[[nodiscard]] constexpr PrimitiveMetadata primitiveMetadata(int type) noexcept {
  switch (type) {
  case ONDA_PRIMITIVE_BOOL:
    return {"bool", 1U};
  case ONDA_PRIMITIVE_F32:
    return {"f32", 4U};
  case ONDA_PRIMITIVE_F64:
    return {"f64", 8U};
  case ONDA_PRIMITIVE_I32:
    return {"i32", 4U};
  case ONDA_PRIMITIVE_I64:
    return {"i64", 8U};
  default:
    return {};
  }
}

[[nodiscard]] constexpr std::string_view primitiveName(int type) noexcept {
  return primitiveMetadata(type).name;
}

[[nodiscard]] constexpr std::size_t primitiveBytes(int type) noexcept {
  return primitiveMetadata(type).bytes;
}

} // namespace onda::plugin
