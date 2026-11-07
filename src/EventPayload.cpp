#include "EventPayload.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>
#include <ranges>
#include <utility>

namespace onda::plugin {
namespace {

using Kind = PayloadType::Kind;

juce::var property(const juce::DynamicObject &object, const char *name) {
  return object.getProperties().getWithDefault(name, juce::var::undefined());
}

std::optional<std::string> stringProperty(const juce::DynamicObject &object,
                                          const char *name) {
  const auto &value = property(object, name);
  if (!value.isString())
    return std::nullopt;
  return value.toString().toStdString();
}

std::optional<PayloadScalar> scalarEncoding(const std::string_view name) {
  if (name == "f32")
    return PayloadScalar::f32;
  if (name == "f64")
    return PayloadScalar::f64;
  if (name == "i32")
    return PayloadScalar::i32;
  if (name == "i64")
    return PayloadScalar::i64;
  if (name == "bool")
    return PayloadScalar::boolean;
  return std::nullopt;
}

std::string_view scalarName(const PayloadScalar scalar) noexcept {
  switch (scalar) {
  case PayloadScalar::f32:
    return "f32";
  case PayloadScalar::f64:
    return "f64";
  case PayloadScalar::i32:
    return "i32";
  case PayloadScalar::i64:
    return "i64";
  case PayloadScalar::boolean:
    return "bool";
  }
  return {};
}

bool parseDefault(const juce::var &input, PayloadDefault &output) {
  if (input.isString()) {
    output.scalar = input.toString().toStdString();
    return true;
  }
  const auto *values = input.getArray();
  if (values == nullptr)
    return false;
  output.elements.reserve(static_cast<std::size_t>(values->size()));
  for (const auto &value : *values) {
    PayloadDefault child;
    if (!parseDefault(value, child))
      return false;
    output.elements.push_back(std::move(child));
  }
  return true;
}

std::optional<PayloadIntegerRange>
parseIntegerRange(const juce::var &input, const PayloadScalar scalar) {
  const auto *range = input.getDynamicObject();
  if (range == nullptr)
    return std::nullopt;
  const auto *minimum = property(*range, "min").getDynamicObject();
  const auto *maximum = property(*range, "max").getDynamicObject();
  const auto mode = stringProperty(*range, "mode");
  if (minimum == nullptr || maximum == nullptr || !mode)
    return std::nullopt;
  const auto minimumType = stringProperty(*minimum, "type");
  const auto maximumType = stringProperty(*maximum, "type");
  const auto minimumValue = stringProperty(*minimum, "value");
  const auto maximumValue = stringProperty(*maximum, "value");
  const auto expected = scalarName(scalar);
  if (!minimumType || !maximumType || !minimumValue || !maximumValue ||
      *minimumType != expected || *maximumType != expected ||
      (*mode != "clamp" && *mode != "wrap")) {
    return std::nullopt;
  }
  return PayloadIntegerRange{*minimumType, *minimumValue, *maximumValue, *mode};
}

PayloadTypePtr parseType(const juce::var &input, std::string &error);

std::optional<PayloadField> parseField(const juce::var &input,
                                       std::string &error) {
  const auto *object = input.getDynamicObject();
  const auto name =
      object == nullptr ? std::nullopt : stringProperty(*object, "name");
  if (object == nullptr || !name || name->empty()) {
    error = "payload schema contains an invalid field";
    return std::nullopt;
  }
  auto type = parseType(property(*object, "ty"), error);
  if (!type)
    return std::nullopt;
  std::optional<PayloadDefault> defaultValue;
  const auto &rawDefault = property(*object, "default");
  if (!rawDefault.isVoid() && !rawDefault.isUndefined()) {
    defaultValue.emplace();
    if (!parseDefault(rawDefault, *defaultValue)) {
      error = "payload schema contains an invalid default";
      return std::nullopt;
    }
  }
  return PayloadField{*name, std::move(type), std::move(defaultValue)};
}

PayloadTypePtr parseType(const juce::var &input, std::string &error) {
  const auto *object = input.getDynamicObject();
  const auto kind =
      object == nullptr ? std::nullopt : stringProperty(*object, "kind");
  if (object == nullptr || !kind) {
    error = "payload schema contains an invalid type";
    return {};
  }

  auto result = std::make_shared<PayloadType>();
  if (*kind == "scalar") {
    const auto encoding = stringProperty(*object, "encoding");
    const auto scalar = encoding ? scalarEncoding(*encoding) : std::nullopt;
    if (!scalar) {
      error = "payload schema contains an invalid scalar";
      return {};
    }
    result->kind = Kind::scalar;
    result->scalar = *scalar;
    const auto &rawEnum = property(*object, "enumeration");
    if (!rawEnum.isVoid() && !rawEnum.isUndefined()) {
      result->enumeration = parseEnumMetadata(rawEnum);
      if (*scalar != PayloadScalar::i32 || !result->enumeration) {
        error = "payload schema contains invalid enum metadata";
        return {};
      }
    }
    const auto &rawRange = property(*object, "integer_range");
    if (!rawRange.isVoid() && !rawRange.isUndefined()) {
      auto range = parseIntegerRange(rawRange, *scalar);
      if (!range || range->scalar.empty()) {
        error = "payload schema contains an invalid integer range";
        return {};
      }
      result->integerRange = std::move(range);
    }
    return result;
  }

  if (*kind == "tuple") {
    const auto *elements = property(*object, "elements").getArray();
    if (elements == nullptr || elements->isEmpty()) {
      error = "payload schema contains an invalid tuple";
      return {};
    }
    result->kind = Kind::tuple;
    result->elements.reserve(static_cast<std::size_t>(elements->size()));
    for (const auto &element : *elements) {
      auto parsed = parseType(element, error);
      if (!parsed || parsed->kind != Kind::scalar)
        return {};
      result->elements.push_back(std::move(parsed));
    }
    return result;
  }

  if (*kind == "struct") {
    const auto name = stringProperty(*object, "name");
    const auto *fields = property(*object, "fields").getArray();
    if (!name || name->empty() || fields == nullptr || fields->isEmpty()) {
      error = "payload schema contains an invalid struct";
      return {};
    }
    result->kind = Kind::structure;
    result->name = *name;
    result->fields.reserve(static_cast<std::size_t>(fields->size()));
    for (const auto &field : *fields) {
      auto parsed = parseField(field, error);
      if (!parsed || std::ranges::any_of(result->fields, [&](const auto &item) {
            return item.name == parsed->name;
          })) {
        if (error.empty())
          error = "payload schema contains duplicate struct fields";
        return {};
      }
      result->fields.push_back(std::move(*parsed));
    }
    return result;
  }

  if (*kind != "array" && *kind != "slice") {
    error = "payload schema contains an unknown type";
    return {};
  }
  auto element = parseType(property(*object, "element"), error);
  if (!element ||
      (element->kind != Kind::scalar && element->kind != Kind::structure)) {
    if (error.empty())
      error = "payload array has an invalid element type";
    return {};
  }
  result->element = std::move(element);
  if (*kind == "slice") {
    result->kind = Kind::slice;
    return result;
  }
  const auto &length = property(*object, "len");
  const auto numericLength = static_cast<juce::int64>(length);
  if ((!length.isInt() && !length.isInt64()) || numericLength <= 0 ||
      static_cast<std::uint64_t>(numericLength) >
          std::numeric_limits<std::size_t>::max()) {
    error = "payload schema contains an invalid array length";
    return {};
  }
  result->kind = Kind::array;
  result->arrayLength = static_cast<std::size_t>(numericLength);
  return result;
}

juce::var object() { return juce::var{new juce::DynamicObject}; }

void set(juce::var &target, const char *name, juce::var value) {
  target.getDynamicObject()->setProperty(name, std::move(value));
}

juce::var defaultSchemaValue(const PayloadDefault &value) {
  if (value.scalar)
    return juce::String(*value.scalar);
  juce::Array<juce::var> result;
  result.ensureStorageAllocated(static_cast<int>(value.elements.size()));
  for (const auto &child : value.elements)
    result.add(defaultSchemaValue(child));
  return result;
}

std::optional<double> parseFiniteDouble(const std::string &text) {
  if (text.empty() || text.front() == '+')
    return std::nullopt;

  juce::CharPointer_UTF8 cursor{text.c_str()};
  if (cursor != cursor.findEndOfWhitespace())
    return std::nullopt;

  const auto value = juce::CharacterFunctions::readDoubleValue(cursor);
  return cursor.isEmpty() && std::isfinite(value) ? std::optional<double>{value}
                                                  : std::nullopt;
}

juce::var defaultScalar(const PayloadScalar scalar,
                        const PayloadDefault *value) {
  const auto text = value != nullptr && value->scalar ? *value->scalar : "0";
  if (scalar == PayloadScalar::boolean)
    return text == "true";
  if (scalar == PayloadScalar::i64)
    return juce::String(text);
  if (scalar == PayloadScalar::i32) {
    std::int32_t parsed{};
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size()
               ? juce::var{static_cast<int>(parsed)}
               : juce::var{0};
  }
  if (text == "NaN")
    return juce::String{"NaN"};
  if (text == "inf" || text == "Infinity")
    return juce::String{"Infinity"};
  if (text == "-inf" || text == "-Infinity")
    return juce::String{"-Infinity"};
  const auto parsed = parseFiniteDouble(text);
  return parsed ? juce::var{*parsed} : juce::var{0.0};
}

const PayloadDefault *defaultChild(const PayloadDefault *value,
                                   const std::size_t index) {
  return value != nullptr && index < value->elements.size()
             ? &value->elements[index]
             : nullptr;
}

juce::var makeDefault(const PayloadType &type, const PayloadDefault *value) {
  if (type.kind == Kind::scalar)
    return defaultScalar(type.scalar, value);
  if (type.kind == Kind::slice)
    return juce::Array<juce::var>{};
  if (type.kind == Kind::structure) {
    auto result = object();
    for (std::size_t index = 0; index < type.fields.size(); ++index) {
      const auto &field = type.fields[index];
      const auto *child = defaultChild(value, index);
      if (child == nullptr && field.defaultValue)
        child = &*field.defaultValue;
      set(result, field.name.c_str(), makeDefault(*field.type, child));
    }
    return result;
  }

  const auto count =
      type.kind == Kind::array ? type.arrayLength : type.elements.size();
  juce::Array<juce::var> result;
  result.ensureStorageAllocated(static_cast<int>(count));
  for (std::size_t index = 0; index < count; ++index) {
    const auto &childType =
        type.kind == Kind::array ? *type.element : *type.elements[index];
    result.add(makeDefault(childType, defaultChild(value, index)));
  }
  return result;
}

template <typename Unsigned>
bool appendLittleEndian(std::span<std::byte> &output, const Unsigned value) {
  if (output.size() < sizeof(value))
    return false;
  for (std::size_t index = 0; index < sizeof(value); ++index)
    output[index] =
        static_cast<std::byte>((value >> (index * 8U)) & Unsigned{0xff});
  output = output.subspan(sizeof(value));
  return true;
}

std::optional<double> finiteNumber(const juce::var &value) {
  if (!value.isInt() && !value.isInt64() && !value.isDouble())
    return std::nullopt;
  const auto number = static_cast<double>(value);
  return std::isfinite(number) ? std::optional<double>{number} : std::nullopt;
}

std::optional<double> floatNumber(const juce::var &value) {
  if (const auto number = finiteNumber(value))
    return number;
  if (!value.isString())
    return std::nullopt;
  const auto text = value.toString();
  if (text == "NaN")
    return std::numeric_limits<double>::quiet_NaN();
  if (text == "Infinity")
    return std::numeric_limits<double>::infinity();
  if (text == "-Infinity")
    return -std::numeric_limits<double>::infinity();
  if (text == "-0")
    return -0.0;
  return std::nullopt;
}

bool integer(const double value) noexcept {
  return std::isfinite(value) && std::trunc(value) == value;
}

bool appendScalar(std::span<std::byte> &output, const PayloadScalar scalar,
                  const juce::var &value) {
  if (scalar == PayloadScalar::boolean)
    return value.isBool() &&
           appendLittleEndian(
               output, static_cast<std::uint8_t>(static_cast<bool>(value)));
  if (scalar == PayloadScalar::i64) {
    std::int64_t parsed{};
    if (value.isString()) {
      const auto text = value.toString().toStdString();
      const auto result =
          std::from_chars(text.data(), text.data() + text.size(), parsed);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return false;
    } else if (value.isInt()) {
      parsed = static_cast<int>(value);
    } else if (value.isInt64()) {
      parsed = static_cast<juce::int64>(value);
    } else {
      const auto number = finiteNumber(value);
      constexpr auto exact = 9'007'199'254'740'991.0;
      if (!number || !integer(*number) || *number < -exact || *number > exact)
        return false;
      parsed = static_cast<std::int64_t>(*number);
    }
    return appendLittleEndian(output, std::bit_cast<std::uint64_t>(parsed));
  }
  if (scalar == PayloadScalar::i32) {
    const auto number = finiteNumber(value);
    if (!number || !integer(*number) ||
        *number <
            static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        *number > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
      return false;
    return appendLittleEndian(output, std::bit_cast<std::uint32_t>(
                                          static_cast<std::int32_t>(*number)));
  }
  const auto number = floatNumber(value);
  if (!number)
    return false;
  if (scalar == PayloadScalar::f32) {
    if (std::isfinite(*number) &&
        (*number < -static_cast<double>(std::numeric_limits<float>::max()) ||
         *number > static_cast<double>(std::numeric_limits<float>::max())))
      return false;
    return appendLittleEndian(
        output, std::bit_cast<std::uint32_t>(static_cast<float>(*number)));
  }
  return appendLittleEndian(output, std::bit_cast<std::uint64_t>(*number));
}

std::size_t leafCount(const PayloadType &type) {
  if (type.kind == Kind::scalar)
    return 1U;
  if (type.kind == Kind::array || type.kind == Kind::slice)
    return leafCount(*type.element);
  if (type.kind == Kind::structure) {
    return std::accumulate(type.fields.begin(), type.fields.end(),
                           std::size_t{0},
                           [](const auto count, const auto &field) {
                             return count + leafCount(*field.type);
                           });
  }
  return std::accumulate(type.elements.begin(), type.elements.end(),
                         std::size_t{0},
                         [](const auto count, const auto &element) {
                           return count + leafCount(*element);
                         });
}

bool encodeValue(const PayloadType &type, const juce::var &value,
                 const std::span<std::span<std::byte>> leaves,
                 std::size_t &leaf) {
  if (type.kind == Kind::scalar)
    return leaf < leaves.size() &&
           appendScalar(leaves[leaf++], type.scalar, value);

  if (type.kind == Kind::structure) {
    const auto *object = value.getDynamicObject();
    if (object == nullptr ||
        object->getProperties().size() != static_cast<int>(type.fields.size()))
      return false;
    for (const auto &field : type.fields) {
      if (!object->hasProperty(field.name.c_str()) ||
          !encodeValue(*field.type, object->getProperty(field.name.c_str()),
                       leaves, leaf))
        return false;
    }
    return true;
  }

  const auto *values = value.getArray();
  const auto expected =
      type.kind == Kind::tuple
          ? type.elements.size()
          : (type.kind == Kind::array ? type.arrayLength
             : values == nullptr ? 0U
                                 : static_cast<std::size_t>(values->size()));
  if (values == nullptr || static_cast<std::size_t>(values->size()) != expected)
    return false;
  if (type.kind == Kind::tuple) {
    for (std::size_t index = 0; index < expected; ++index) {
      if (!encodeValue(*type.elements[index],
                       values->getReference(static_cast<int>(index)), leaves,
                       leaf))
        return false;
    }
    return true;
  }

  const auto start = leaf;
  const auto count = leafCount(*type.element);
  for (const auto &item : *values) {
    auto current = start;
    if (!encodeValue(*type.element, item, leaves, current) ||
        current != start + count)
      return false;
  }
  leaf = start + count;
  return true;
}

} // namespace

bool parsePayloadSchema(const std::string_view json, PayloadSchema &schema,
                        std::string &error) {
  schema = {};
  error.clear();
  juce::var root;
  const auto parsed = juce::JSON::parse(
      juce::String::fromUTF8(json.data(), static_cast<int>(json.size())), root);
  const auto *object = root.getDynamicObject();
  const auto *parameters =
      object == nullptr ? nullptr : property(*object, "params").getArray();
  if (parsed.failed() || parameters == nullptr) {
    error = "Onda returned invalid payload schema JSON";
    return false;
  }
  schema.parameters.reserve(static_cast<std::size_t>(parameters->size()));
  for (const auto &rawParameter : *parameters) {
    auto field = parseField(rawParameter, error);
    if (!field || std::ranges::any_of(schema.parameters, [&](const auto &item) {
          return item.name == field->name;
        })) {
      if (error.empty())
        error = "payload schema contains duplicate parameters";
      schema = {};
      return false;
    }
    schema.parameters.push_back({std::move(field->name), std::move(field->type),
                                 std::move(field->defaultValue)});
  }
  return true;
}

std::string payloadTypeName(const PayloadType &type) {
  if (type.kind == Kind::scalar)
    return std::string{scalarName(type.scalar)};
  if (type.kind == Kind::structure)
    return type.name;
  if (type.kind == Kind::array)
    return payloadTypeName(*type.element) + "[" +
           std::to_string(type.arrayLength) + "]";
  if (type.kind == Kind::slice)
    return payloadTypeName(*type.element) + "[]";
  std::string result{"("};
  for (std::size_t index = 0; index < type.elements.size(); ++index) {
    if (index != 0U)
      result += ", ";
    result += payloadTypeName(*type.elements[index]);
  }
  return result + ")";
}

juce::var payloadTypeValue(const PayloadType &type) {
  auto result = object();
  if (type.kind == Kind::scalar) {
    set(result, "kind", "scalar");
    set(result, "encoding", juce::String(scalarName(type.scalar).data()));
    if (type.enumeration)
      set(result, "enumeration", enumMetadataValue(*type.enumeration));
    if (type.integerRange) {
      auto range = object();
      auto minimum = object();
      set(minimum, "type", juce::String(type.integerRange->scalar));
      set(minimum, "value", juce::String(type.integerRange->minimum));
      auto maximum = object();
      set(maximum, "type", juce::String(type.integerRange->scalar));
      set(maximum, "value", juce::String(type.integerRange->maximum));
      set(range, "min", std::move(minimum));
      set(range, "max", std::move(maximum));
      set(range, "mode", juce::String(type.integerRange->mode));
      set(result, "integer_range", std::move(range));
    }
    return result;
  }
  if (type.kind == Kind::tuple) {
    set(result, "kind", "tuple");
    juce::Array<juce::var> elements;
    for (const auto &element : type.elements)
      elements.add(payloadTypeValue(*element));
    set(result, "elements", std::move(elements));
    return result;
  }
  if (type.kind == Kind::structure) {
    set(result, "kind", "struct");
    set(result, "name", juce::String(type.name));
    juce::Array<juce::var> fields;
    for (const auto &field : type.fields) {
      auto value = object();
      set(value, "name", juce::String(field.name));
      set(value, "ty", payloadTypeValue(*field.type));
      if (field.defaultValue)
        set(value, "default", defaultSchemaValue(*field.defaultValue));
      fields.add(std::move(value));
    }
    set(result, "fields", std::move(fields));
    return result;
  }
  set(result, "kind", type.kind == Kind::array ? "array" : "slice");
  set(result, "element", payloadTypeValue(*type.element));
  if (type.kind == Kind::array)
    set(result, "len", static_cast<juce::int64>(type.arrayLength));
  return result;
}

juce::var
payloadDefaultValue(const PayloadType &type,
                    const std::optional<PayloadDefault> &defaultValue) {
  return makeDefault(type, defaultValue ? &*defaultValue : nullptr);
}

bool encodePayload(const PayloadSchema &schema, const onda_payload_plan_t *plan,
                   const juce::var &values,
                   const std::span<std::byte> destination, std::size_t &written,
                   std::string &error) {
  written = 0U;
  error.clear();
  const auto *arguments = values.getArray();
  if (arguments == nullptr ||
      static_cast<std::size_t>(arguments->size()) != schema.parameters.size()) {
    error = "invalid argument count";
    return false;
  }

  std::vector<std::int32_t> lengths;
  for (std::size_t index = 0; index < schema.parameters.size(); ++index) {
    if (schema.parameters[index].type->kind != Kind::slice)
      continue;
    const auto *items =
        arguments->getReference(static_cast<int>(index)).getArray();
    if (items == nullptr) {
      error = "parameter '" + schema.parameters[index].name +
              "' has an invalid value";
      return false;
    }
    lengths.push_back(items->size());
  }
  onda_payload_layout_t layout{};
  if (onda_payload_layout(plan, lengths.data(),
                          static_cast<int>(lengths.size()), nullptr, 0,
                          &layout) != 0) {
    error = "invalid event payload layout";
    return false;
  }
  if (static_cast<std::size_t>(layout.payload_bytes) > destination.size()) {
    error = "event payload exceeds the host capacity";
    return false;
  }
  std::vector<onda_payload_region_t> regions(
      static_cast<std::size_t>(layout.region_count));
  if (onda_payload_layout(plan, lengths.data(),
                          static_cast<int>(lengths.size()), regions.data(),
                          layout.region_count, &layout) != 0) {
    error = "invalid event payload layout";
    return false;
  }
  std::vector<std::span<std::byte>> leaves;
  leaves.reserve(regions.size() - lengths.size());
  std::size_t lengthIndex{};
  for (const auto &region : regions) {
    auto bytes =
        destination.subspan(static_cast<std::size_t>(region.payload_offset),
                            static_cast<std::size_t>(region.payload_bytes));
    if (region.tensor_index < 0) {
      if (!appendLittleEndian(
              bytes, static_cast<std::uint32_t>(lengths[lengthIndex++]))) {
        error = "invalid event slice prefix";
        return false;
      }
    } else {
      leaves.push_back(bytes);
    }
  }
  std::size_t leaf{};
  for (std::size_t index = 0; index < schema.parameters.size(); ++index) {
    const auto &parameter = schema.parameters[index];
    if (!encodeValue(*parameter.type,
                     arguments->getReference(static_cast<int>(index)), leaves,
                     leaf)) {
      error = "parameter '" + parameter.name + "' has an invalid value";
      return false;
    }
  }
  if (leaf != leaves.size() ||
      std::ranges::any_of(leaves,
                          [](const auto bytes) { return !bytes.empty(); })) {
    error = "event values do not match the payload layout";
    return false;
  }
  written = static_cast<std::size_t>(layout.payload_bytes);
  return true;
}

} // namespace onda::plugin
