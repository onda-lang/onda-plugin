#pragma once

#include "ScalarValue.h"

#include <cstdint>
#include <juce_core/juce_core.h>
#include <limits>

namespace onda::plugin {

inline constexpr std::uint32_t stateMagic = 0x41444e4fU; // ONDA, little-endian.
inline constexpr int stateVersion = 12;

inline void writeScalarValue(juce::OutputStream &stream,
                             const ScalarValue &value) {
  stream.writeDouble(value.normalized);
  stream.writeBool(value.scalarDefault.has_value());
  if (value.scalarDefault) {
    const auto &domain = value.scalarDefault->domain;
    const auto &plain = value.scalarDefault->plain;
    stream.writeBool(std::holds_alternative<std::int64_t>(plain));
    stream.writeInt64(std::bit_cast<std::int64_t>(scalarPlainBits(plain)));
    stream.writeDouble(domain.minimum);
    stream.writeDouble(domain.maximum);
    stream.writeDouble(domain.step);
    stream.writeDouble(domain.curve);
    stream.writeInt(static_cast<int>(domain.stepCount));
    stream.writeByte(static_cast<char>(domain.type));
    stream.writeBool(domain.logarithmic);
    stream.writeBool(domain.hasCurve);
    stream.writeBool(domain.defaultMayExceedRange);
  }
}

inline std::optional<ScalarValue> readScalarValue(juce::InputStream &stream) {
  if (stream.getNumBytesRemaining() < 9)
    return std::nullopt;
  ScalarValue value{stream.readDouble()};
  if (stream.readBool()) {
    if (stream.getNumBytesRemaining() < 49)
      return std::nullopt;
    const auto isInteger = stream.readBool();
    const auto plain =
        scalarPlainFromBits(std::bit_cast<std::uint64_t>(
                                static_cast<std::int64_t>(stream.readInt64())),
                            isInteger);
    const auto number = scalarPlainNumber(plain);
    ScalarDomain domain;
    domain.minimum = stream.readDouble();
    domain.maximum = stream.readDouble();
    domain.step = stream.readDouble();
    domain.curve = stream.readDouble();
    domain.stepCount = static_cast<std::uint32_t>(stream.readInt());
    domain.type = static_cast<ScalarDomain::Type>(stream.readByte());
    domain.logarithmic = stream.readBool();
    domain.hasCurve = stream.readBool();
    domain.defaultMayExceedRange = stream.readBool();
    const auto f32 = domain.type == ScalarDomain::Type::f32;
    const auto lower =
        f32 ? static_cast<double>(static_cast<float>(domain.minimum))
            : domain.minimum;
    const auto upper =
        f32 ? static_cast<double>(static_cast<float>(domain.maximum))
            : domain.maximum;
    const auto i32 = domain.type == ScalarDomain::Type::i32;
    const auto i64 = domain.type == ScalarDomain::Type::i64;
    const auto *integer = std::get_if<std::int64_t>(&plain);
    const auto integerFits =
        integer
            ? i64 || (i32 &&
                      *integer >= std::numeric_limits<std::int32_t>::min() &&
                      *integer <= std::numeric_limits<std::int32_t>::max())
            : (!i32 && !i64) ||
                  (std::is_eq(std::trunc(number) <=> number) &&
                   (i32 ? number >= std::numeric_limits<std::int32_t>::min() &&
                              number <= std::numeric_limits<std::int32_t>::max()
                        : number >= -std::ldexp(1.0, 63) &&
                              number < std::ldexp(1.0, 63)));
    if (domain.type > ScalarDomain::Type::boolean || !std::isfinite(number) ||
        !std::isfinite(domain.minimum) || !std::isfinite(domain.maximum) ||
        domain.minimum > domain.maximum || !std::isfinite(domain.step) ||
        domain.step < 0.0 || !std::isfinite(domain.curve) ||
        !std::isfinite(lower) || !std::isfinite(upper) ||
        (f32 && !std::isfinite(static_cast<float>(number))) || !integerFits ||
        (domain.type == ScalarDomain::Type::boolean &&
         !std::is_eq(number <=> 0.0) && !std::is_eq(number <=> 1.0)) ||
        (!domain.defaultMayExceedRange && (number < lower || number > upper)) ||
        (domain.logarithmic && domain.minimum <= 0.0))
      return std::nullopt;
    value.scalarDefault = ScalarValue::ScalarDefault{plain, domain};
  }
  return value;
}

} // namespace onda::plugin
