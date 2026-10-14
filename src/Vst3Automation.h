#pragma once

#include <cstdint>

namespace juce {
class AudioProcessor;
class AudioProcessorParameter;
class String;
} // namespace juce
namespace Steinberg::Vst {
struct ProcessData;
}

namespace onda::plugin {
class Processor;
juce::String vst3BusName(juce::AudioProcessor &processor, bool input,
                         int index);
bool vst3OwnsParameter(juce::AudioProcessor &processor,
                       std::uint32_t id) noexcept;
bool vst3IsHostParameterNotification(
    const juce::AudioProcessorParameter *parameter) noexcept;
double vst3NormalizedToPlain(const juce::AudioProcessorParameter &parameter,
                             double value);
double vst3PlainToNormalized(const juce::AudioProcessorParameter &parameter,
                             double value);
std::int32_t vst3ParameterStepCount(
    const juce::AudioProcessorParameter &parameter, std::int32_t fallback);

// The generated wrapper borrows host queues only for this processing call.
class ScopedVst3Automation final {
public:
  ScopedVst3Automation(juce::AudioProcessor &processor,
                       Steinberg::Vst::ProcessData &data) noexcept;
  ~ScopedVst3Automation();
  void publishTargets() noexcept;
  ScopedVst3Automation(const ScopedVst3Automation &) = delete;
  ScopedVst3Automation &operator=(const ScopedVst3Automation &) = delete;

private:
  Processor &processor_;
};
} // namespace onda::plugin
