#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace juce {
class var;
}

namespace onda::plugin {

enum class PayloadScalar : std::uint8_t { f32, f64, i32, i64, boolean };

struct PayloadDefault {
  std::optional<std::string> scalar;
  std::vector<PayloadDefault> elements;
};

struct PayloadType;
using PayloadTypePtr = std::shared_ptr<const PayloadType>;

struct PayloadField {
  std::string name;
  PayloadTypePtr type;
  std::optional<PayloadDefault> defaultValue;
};

struct PayloadIntegerRange {
  std::string scalar;
  std::string minimum;
  std::string maximum;
  std::string mode;
};

struct PayloadType {
  enum class Kind : std::uint8_t { scalar, tuple, structure, array, slice };

  Kind kind{};
  PayloadScalar scalar{};
  std::optional<PayloadIntegerRange> integerRange;
  std::string name;
  std::vector<PayloadField> fields;
  std::vector<PayloadTypePtr> elements;
  PayloadTypePtr element;
  std::size_t arrayLength{};
};

struct PayloadParameter {
  std::string name;
  PayloadTypePtr type;
  std::optional<PayloadDefault> defaultValue;
};

struct PayloadSchema {
  std::vector<PayloadParameter> parameters;
};

struct PayloadLeaf {
  std::string path;
  PayloadScalar scalar{};
  std::size_t fixedElements{1U};
  std::vector<std::size_t> shape;
};

struct PayloadParameterPlan {
  bool dynamic{};
  std::vector<PayloadLeaf> leaves;
};

struct PayloadPlan {
  std::vector<PayloadParameterPlan> parameters;
};

[[nodiscard]] bool parsePayloadSchema(std::string_view json,
                                      PayloadSchema &schema,
                                      std::string &error);
[[nodiscard]] std::string payloadTypeName(const PayloadType &type);
[[nodiscard]] juce::var payloadTypeValue(const PayloadType &type);
[[nodiscard]] juce::var
payloadDefaultValue(const PayloadType &type,
                    const std::optional<PayloadDefault> &defaultValue);
[[nodiscard]] bool encodePayload(const PayloadSchema &schema,
                                 const juce::var &values,
                                 std::span<std::byte> destination,
                                 std::size_t &written, std::string &error);
[[nodiscard]] PayloadPlan makePayloadPlan(const PayloadSchema &schema);
[[nodiscard]] std::size_t payloadScalarBytes(PayloadScalar scalar) noexcept;

} // namespace onda::plugin
