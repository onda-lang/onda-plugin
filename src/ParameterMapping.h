#pragma once

#include "ScalarValue.h"

#include <onda_processor_abi.h>

#include <compare>
#include <cstdint>
#include <optional>
#include <string>

namespace onda::plugin {

// Compiled scalar metadata shared by DSP, host parameters and presentation.
struct ParameterMapping {
  int parameterIndex{-1};
  int elementIndex{};
  int arrayLength{};
  std::string arrayName;
  std::string name;
  std::string type;
  std::string unit;
  double defaultNormalized{0.5};
  ScalarPlain defaultPlain{};
  double rangeMin{};
  double rangeMax{};
  std::string scale{"linear"};
  std::optional<double> curve;
  std::optional<double> step;
  std::optional<std::int64_t> stepCount;
  bool defaultMayExceedRange{};

  onda_processor_param_domain hostDomain() const noexcept {
    const auto domain = scalarDomain();
    const auto scalar = domain.type == ScalarDomain::Type::f64
                            ? ONDA_PROCESSOR_PARAM_SCALAR_F64
                        : domain.type == ScalarDomain::Type::i32
                            ? ONDA_PROCESSOR_PARAM_SCALAR_I32
                        : domain.type == ScalarDomain::Type::i64
                            ? ONDA_PROCESSOR_PARAM_SCALAR_I64
                            : ONDA_PROCESSOR_PARAM_SCALAR_F32;
    return {
        .minimum = domain.minimum,
        .maximum = domain.maximum,
        .step = domain.step,
        .curve = domain.curve,
        .step_count = domain.stepCount,
        .scale = domain.logarithmic ? ONDA_PROCESSOR_PARAM_SCALE_LOG
                                    : ONDA_PROCESSOR_PARAM_SCALE_LINEAR,
        .scalar = scalar,
        .has_curve = static_cast<std::uint8_t>(domain.hasCurve),
        .unit = nullptr,
    };
  }

  double normalizedToPlain(const double normalized) const noexcept {
    const auto domain = hostDomain();
    return onda_processor_param_normalized_to_plain(&domain, normalized);
  }

  double plainToNormalized(const double plain) const noexcept {
    const auto domain = hostDomain();
    return onda_processor_param_plain_to_normalized(&domain, plain);
  }

  ScalarValue controlDefault() const noexcept {
    ScalarValue value{defaultNormalized};
    value.scalarDefault = ScalarValue::ScalarDefault{
        static_cast<float>(scalarPlainNumber(defaultPlain)),
        {rangeMin, rangeMax}};
    return value;
  }

  ScalarDomain scalarDomain() const noexcept {
    const auto scalar = type == "f64"    ? ScalarDomain::Type::f64
                        : type == "i32"  ? ScalarDomain::Type::i32
                        : type == "i64"  ? ScalarDomain::Type::i64
                        : type == "bool" ? ScalarDomain::Type::boolean
                                         : ScalarDomain::Type::f32;
    return {rangeMin,
            rangeMax,
            step.value_or(0.0),
            curve.value_or(0.0),
            static_cast<std::uint32_t>(stepCount.value_or(0)),
            scalar,
            scale == "log",
            curve.has_value(),
            defaultMayExceedRange};
  }

  ScalarValue parameterDefault() const noexcept {
    ScalarValue value{defaultNormalized};
    value.scalarDefault =
        ScalarValue::ScalarDefault{defaultPlain, scalarDomain()};
    return value;
  }

  friend bool operator==(const ParameterMapping &left,
                         const ParameterMapping &right) noexcept {
    const auto equalDouble = [](const double a, const double b) noexcept {
      return std::is_eq(a <=> b);
    };
    const auto equalOptionalDouble =
        [&equalDouble](const std::optional<double> &a,
                       const std::optional<double> &b) noexcept {
          return a.has_value() == b.has_value() && (!a || equalDouble(*a, *b));
        };
    return left.parameterIndex == right.parameterIndex &&
           left.elementIndex == right.elementIndex &&
           left.arrayLength == right.arrayLength &&
           left.arrayName == right.arrayName && left.name == right.name &&
           left.type == right.type && left.unit == right.unit &&
           equalDouble(left.defaultNormalized, right.defaultNormalized) &&
           left.defaultPlain == right.defaultPlain &&
           equalDouble(left.rangeMin, right.rangeMin) &&
           equalDouble(left.rangeMax, right.rangeMax) &&
           left.scale == right.scale &&
           equalOptionalDouble(left.curve, right.curve) &&
           equalOptionalDouble(left.step, right.step) &&
           left.stepCount == right.stepCount &&
           left.defaultMayExceedRange == right.defaultMayExceedRange;
  }
};

} // namespace onda::plugin
