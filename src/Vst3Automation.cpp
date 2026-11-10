#include "Vst3Automation.h"
#include "HostParameter.h"
#include "Processor.h"

#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/vst/ivstaudioprocessor.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/vst/ivsteditcontroller.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/vst/ivstparameterchanges.h>

#include <algorithm>
#include <limits>

namespace onda::plugin {
namespace {
// Retain JUCE's boolean presentation and wrapper-owned state. Host queues only
// commit its atomic value; controller/listener delivery belongs to the timer.
class DeferredBypassParameter final : public juce::AudioParameterBool,
                                      private juce::Timer {
public:
  DeferredBypassParameter() : AudioParameterBool("byps", "Bypass", false) {
    startTimerHz(20);
  }
  ~DeferredBypassParameter() override { stopTimer(); }

  void setFromHost(const float newValue) noexcept {
    auto &parameter = static_cast<juce::AudioProcessorParameter &>(*this);
    if (!juce::approximatelyEqual(parameter.getValue(), newValue)) {
      parameter.setValue(newValue);
      pending_.store(true, std::memory_order_release);
    }
  }

private:
  void timerCallback() override {
    if (pending_.exchange(false, std::memory_order_acq_rel))
      HostParameter::notifyHostValue(*this);
  }
  std::atomic<bool> pending_{};
};

bool readPoint(const void *context, const int index,
               AutomationPoint &point) noexcept {
  auto *queue = static_cast<Steinberg::Vst::IParamValueQueue *>(
      const_cast<void *>(context));
  Steinberg::int32 offset{};
  const auto result = queue->getPoint(index, offset, point.value);
  point.offset = static_cast<int>(offset);
  return result == Steinberg::kResultOk;
}
} // namespace

std::unique_ptr<juce::AudioProcessorParameter> makeVst3BypassParameter() {
  return std::make_unique<DeferredBypassParameter>();
}

bool vst3SetBypassFromHost(juce::AudioProcessorParameter &parameter,
                           const float value) noexcept {
  if (auto *bypass = dynamic_cast<DeferredBypassParameter *>(&parameter)) {
    bypass->setFromHost(value);
    return true;
  }
  return false;
}

void vst3MidiOverflow(juce::AudioProcessor &processor) noexcept {
  static_cast<Processor &>(processor).requestHostNotesOff();
}

bool vst3OwnsParameter(juce::AudioProcessor &processor,
                       const std::uint32_t id) noexcept {
  return static_cast<Processor &>(processor).ownsHostParameter(id);
}

bool vst3IsHostParameterNotification(
    const juce::AudioProcessorParameter *parameter) noexcept {
  return HostParameter::isHostValueNotification(parameter);
}

double vst3NormalizedToPlain(const juce::AudioProcessorParameter &parameter,
                             const double value) {
  const auto *host = dynamic_cast<const HostParameter *>(&parameter);
  return host != nullptr ? host->normalizedToPlain(value) : value;
}

double vst3PlainToNormalized(const juce::AudioProcessorParameter &parameter,
                             const double value) {
  const auto *host = dynamic_cast<const HostParameter *>(&parameter);
  return host != nullptr ? host->plainToNormalized(value) : value;
}

std::int32_t vst3ParameterStepCount(
    const juce::AudioProcessorParameter &parameter, const std::int32_t fallback) {
  const auto *host = dynamic_cast<const HostParameter *>(&parameter);
  return host != nullptr
             ? static_cast<std::int32_t>(std::min<std::int64_t>(
                   host->discreteStepCount(),
                   std::numeric_limits<std::int32_t>::max()))
             : fallback;
}

std::int32_t vst3ParameterFlags(
    const juce::AudioProcessorParameter &parameter, const std::int32_t fallback) {
  using Flags = Steinberg::Vst::ParameterInfo;
  const auto *host = dynamic_cast<const HostParameter *>(&parameter);
  if (host == nullptr)
    return fallback;
  auto flags = fallback & ~(Flags::kIsList | Flags::kIsReadOnly | Flags::kCanAutomate);
  flags |= host->isAutomatable() ? Flags::kCanAutomate : Flags::kIsReadOnly;
  if (host->isEnum())
    flags |= Flags::kIsList;
  return flags;
}

juce::String vst3BusName(juce::AudioProcessor &processor, const bool input,
                         const int index) {
  if (input && index >= pluginMainInputBuses)
    return static_cast<Processor &>(processor).controlInputName(
        static_cast<std::size_t>(index - pluginMainInputBuses));
  return processor.getBus(input, index)->getName();
}

ScopedVst3Automation::ScopedVst3Automation(
    juce::AudioProcessor &processor, Steinberg::Vst::ProcessData &data) noexcept
    : processor_(static_cast<Processor &>(processor)) {
  processor_.clearHostAutomation();
  if (auto *changes = data.inputParameterChanges) {
    for (int index = 0; index < changes->getParameterCount(); ++index) {
      if (auto *queue = changes->getParameterData(index))
        processor_.captureHostAutomation(
            queue->getParameterId(), {queue, queue->getPointCount(), readPoint},
            data.numSamples);
    }
  }
}

ScopedVst3Automation::~ScopedVst3Automation() {
  processor_.clearHostAutomation();
}

void ScopedVst3Automation::publishTargets() noexcept {
  processor_.publishHostAutomationTargets();
}
} // namespace onda::plugin
