#include "EnumMetadata.h"

#include <juce_core/juce_core.h>

#include <unordered_set>
#include <utility>

namespace onda::plugin {

EnumMetadataPtr parseEnumMetadata(const juce::var &value) {
  if (value.getDynamicObject() == nullptr || !value["name"].isString())
    return {};
  const auto *members = value["members"].getArray();
  if (members == nullptr || members->isEmpty())
    return {};
  auto metadata = std::make_shared<EnumMetadata>();
  metadata->name = value["name"].toString().toStdString();
  if (metadata->name.empty())
    return {};
  std::unordered_set<std::string> names;
  metadata->members.reserve(static_cast<std::size_t>(members->size()));
  for (const auto &member : *members) {
    if (member.getDynamicObject() == nullptr || !member["name"].isString() ||
        !member["label"].isString())
      return {};
    auto name = member["name"].toString().toStdString();
    if (name.empty() || !names.insert(name).second)
      return {};
    metadata->members.push_back(
        {std::move(name), member["label"].toString().toStdString()});
  }
  return metadata;
}

juce::var enumMetadataValue(const EnumMetadata &metadata) {
  juce::var result{new juce::DynamicObject};
  auto *object = result.getDynamicObject();
  object->setProperty("name", juce::String(metadata.name));
  juce::Array<juce::var> members;
  members.ensureStorageAllocated(static_cast<int>(metadata.members.size()));
  for (const auto &member : metadata.members) {
    juce::var value{new juce::DynamicObject};
    value.getDynamicObject()->setProperty("name", juce::String(member.name));
    value.getDynamicObject()->setProperty("label", juce::String(member.label));
    members.add(std::move(value));
  }
  object->setProperty("members", std::move(members));
  return result;
}

} // namespace onda::plugin
