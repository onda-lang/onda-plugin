#pragma once

#include <memory>
#include <string>
#include <vector>

namespace juce {
class var;
}

namespace onda::plugin {

struct EnumMember {
  std::string name;
  std::string label;
  bool operator==(const EnumMember &) const = default;
};

struct EnumMetadata {
  std::string name;
  std::vector<EnumMember> members;
  bool operator==(const EnumMetadata &) const = default;
};

using EnumMetadataPtr = std::shared_ptr<const EnumMetadata>;

[[nodiscard]] EnumMetadataPtr parseEnumMetadata(const juce::var &value);
[[nodiscard]] juce::var enumMetadataValue(const EnumMetadata &metadata);

} // namespace onda::plugin
