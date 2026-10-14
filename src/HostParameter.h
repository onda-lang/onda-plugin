#pragma once

#include "ParameterMapping.h"
#include "ParameterState.h"
#include <cmath>
#include <juce_audio_processors/juce_audio_processors.h>
#include <limits>
#include <memory>
#include <mutex>
#include <onda_processor_abi.h>

namespace onda::plugin {

class HostParameter final : public juce::RangedAudioParameter {
public:
  HostParameter(const juce::String &id, const juce::String &fallback,
                ParameterValue &value, const bool showSlot)
      : RangedAudioParameter(juce::ParameterID{id, 1}, fallback),
        fallbackName_(fallback), value_(value), showSlotSuffix_(showSlot) {}

  void setEditorValue(const float value) {
    value_.set(value, true);
    notifyEditorValue();
  }

  void notifyEditorValue() {
    const juce::ScopedValueSetter<const juce::AudioProcessorParameter *>
        notification(hostValueNotification_, nullptr);
    sendValueChangedMessageToListeners(getValue());
  }

  // Synchronous listeners retain the host origin, including controller
  // presentation updates. Nested editor changes establish their own origin.
  void notifyHostValue() {
    const juce::ScopedValueSetter<const juce::AudioProcessorParameter *>
        notification(hostValueNotification_, this);
    sendValueChangedMessageToListeners(getValue());
  }

  [[nodiscard]] static bool isHostValueNotification(
      const juce::AudioProcessorParameter *parameter) noexcept {
    return parameter != nullptr && hostValueNotification_ == parameter;
  }

  float getValue() const override { return static_cast<float>(value_.value()); }
  void setValue(const float value) override { value_.set(value); }
  const juce::NormalisableRange<float> &getNormalisableRange() const override {
    return normalizedRange_;
  }

  bool setMapping(const ParameterMapping *const mapping) {
    std::lock_guard lock(mappingMutex_);
    if ((mapping == nullptr && !mapping_) ||
        (mapping != nullptr && mapping_ && *mapping == *mapping_)) {
      return false;
    }
    mapping_ = mapping == nullptr
                   ? std::shared_ptr<const ParameterMapping>{}
                   : std::make_shared<const ParameterMapping>(*mapping);
    return true;
  }

  juce::String getName(const int maximumLength) const override {
    const auto mapping = mappingSnapshot();
    if (mapping && (!showSlotSuffix_ || maximumLength <= 8))
      return juce::String(mapping->name).substring(0, maximumLength);
    const auto displayName =
        mapping ? juce::String(mapping->name) + " [" + fallbackName_ + "]"
                : fallbackName_;
    return displayName.substring(0, maximumLength);
  }

  juce::String getLabel() const override {
    const auto mapping = mappingSnapshot();
    return mapping ? juce::String(mapping->unit) : juce::String{};
  }

  float getDefaultValue() const override {
    const auto mapping = mappingSnapshot();
    return mapping ? static_cast<float>(mapping->defaultNormalized) : 0.5F;
  }

  int getNumSteps() const override {
    const auto count = discreteStepCount();
    if (count == 0)
      return juce::AudioProcessorParameter::getDefaultNumParameterSteps();
    // INT_MAX is JUCE's continuous sentinel. VST3 receives the exact count
    // separately, without passing through this narrower number of values.
    constexpr auto maximum = std::numeric_limits<int>::max() - 1;
    return static_cast<int>(std::min<std::int64_t>(count, maximum - 1) + 1);
  }

  std::int64_t discreteStepCount() const {
    const auto mapping = mappingSnapshot();
    if (!mapping)
      return 0;
    return mapping->type == "bool" ? 1 : mapping->stepCount.value_or(0);
  }

  bool isDiscrete() const override {
    const auto mapping = mappingSnapshot();
    return mapping && (mapping->type == "bool" || mapping->stepCount);
  }

  bool isBoolean() const override {
    const auto mapping = mappingSnapshot();
    return mapping && mapping->type == "bool";
  }

  double normalizedToPlain(const double value) const {
    const auto mapping = mappingSnapshot();
    return mapping ? normalizedToPlain(*mapping, value) : value;
  }

  double plainToNormalized(const double value) const {
    const auto mapping = mappingSnapshot();
    return mapping ? plainToNormalized(*mapping, value) : value;
  }

  juce::String getText(const float normalizedValue,
                       const int maximumLength) const override {
    const auto mapping = mappingSnapshot();
    if (!mapping)
      return truncate(juce::String(normalizedValue), maximumLength);
    if (mapping->type == "bool")
      return truncate(normalizedValue >= 0.5F ? "On" : "Off", maximumLength);

    const auto plain = normalizedToPlain(*mapping, normalizedValue);
    const auto text =
        mapping->type == "i32" || mapping->type == "i64"
            ? juce::String(static_cast<juce::int64>(std::llround(plain)))
            : juce::String(plain);
    return truncate(text, maximumLength);
  }

  float getValueForText(const juce::String &text) const override {
    const auto mapping = mappingSnapshot();
    if (!mapping)
      return normalizedText(text);
    if (mapping->type == "bool") {
      const auto lowered = text.trim().toLowerCase();
      if (lowered == "on" || lowered == "true" || lowered == "yes")
        return 1.0F;
      if (lowered == "off" || lowered == "false" || lowered == "no")
        return 0.0F;
      return lowered.getDoubleValue() >= 0.5 ? 1.0F : 0.0F;
    }

    return static_cast<float>(
        plainToNormalized(*mapping, text.getDoubleValue()));
  }

private:
  static double normalizedToPlain(const ParameterMapping &mapping,
                                  const double value) noexcept {
    if (mapping.type == "bool")
      return value >= 0.5 ? 1.0 : 0.0;
    return std::is_eq(mapping.rangeMin <=> mapping.rangeMax)
               ? mapping.rangeMin
               : mapping.normalizedToPlain(value);
  }

  static double plainToNormalized(const ParameterMapping &mapping,
                                  const double value) noexcept {
    if (mapping.type == "bool")
      return value >= 0.5 ? 1.0 : 0.0;
    if (std::is_eq(mapping.rangeMin <=> mapping.rangeMax))
      return mapping.defaultNormalized;
    const auto normalized = mapping.plainToNormalized(value);
    return std::isfinite(normalized) ? std::clamp(normalized, 0.0, 1.0) : 0.5;
  }

  [[nodiscard]] std::shared_ptr<const ParameterMapping>
  mappingSnapshot() const {
    std::lock_guard lock(mappingMutex_);
    return mapping_;
  }

  static juce::String truncate(juce::String text, const int maximumLength) {
    return maximumLength > 0 ? text.substring(0, maximumLength) : text;
  }

  static float normalizedText(const juce::String &text) {
    const auto value = text.getDoubleValue();
    return std::isfinite(value)
               ? static_cast<float>(std::clamp(value, 0.0, 1.0))
               : 0.5F;
  }

  const juce::String fallbackName_;
  inline static thread_local const juce::AudioProcessorParameter
      *hostValueNotification_{};
  const juce::NormalisableRange<float> normalizedRange_{0.0F, 1.0F};
  ParameterValue &value_;
  const bool showSlotSuffix_;
  mutable std::mutex mappingMutex_;
  std::shared_ptr<const ParameterMapping> mapping_;
};

} // namespace onda::plugin
