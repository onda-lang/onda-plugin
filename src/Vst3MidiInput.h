#pragma once

#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/vst/ivstevents.h>

#include <algorithm>
#include <cstddef>

namespace onda::plugin {

// One budget covers controller queues and the host event list. JUCE retains
// conversion and stable sorting; only short channel messages reach its buffer.
class Vst3MidiInput final : public Steinberg::Vst::IEventList {
public:
  static constexpr int maximumEvents = 4096;
  // Each accepted message has at most three bytes. Sixteen bytes also cover
  // JUCE's per-event timestamp and length, without relying on capacity
  // rounding.
  static constexpr std::size_t storageBytes = maximumEvents * 16U;

  explicit Vst3MidiInput(Steinberg::Vst::IEventList *events) noexcept
      : events_(events) {}

  int accept(const int count) noexcept {
    if (overflowed_)
      return 0;
    if (count > remaining_) {
      overflowed_ = true;
      return 0;
    }
    const auto accepted = std::max(count, 0);
    remaining_ -= accepted;
    return accepted;
  }

  bool overflowed() const noexcept { return overflowed_; }

  Steinberg::int32 PLUGIN_API getEventCount() override {
    if (eventCount_ < 0)
      eventCount_ = events_ != nullptr ? accept(events_->getEventCount()) : 0;
    return eventCount_;
  }
  Steinberg::tresult PLUGIN_API
  getEvent(Steinberg::int32 index, Steinberg::Vst::Event &event) override {
    if (index < 0 || index >= getEventCount() || events_ == nullptr ||
        events_->getEvent(index, event) != Steinberg::kResultOk)
      return Steinberg::kResultFalse;
    switch (event.type) {
    case Steinberg::Vst::Event::kNoteOnEvent:
    case Steinberg::Vst::Event::kNoteOffEvent:
    case Steinberg::Vst::Event::kPolyPressureEvent:
    case Steinberg::Vst::Event::kLegacyMIDICCOutEvent:
      return Steinberg::kResultOk;
    default:
      // In particular, reject SysEx before constructing an owning MidiMessage.
      return Steinberg::kResultFalse;
    }
  }
  Steinberg::tresult PLUGIN_API addEvent(Steinberg::Vst::Event &) override {
    return Steinberg::kNotImplemented;
  }
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID,
                                               void **object) override {
    if (object == nullptr)
      return Steinberg::kInvalidArgument;
    *object = nullptr;
    return Steinberg::kNoInterface;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
  Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
  Steinberg::Vst::IEventList *events_{};
  int remaining_{maximumEvents};
  int eventCount_{-1};
  bool overflowed_{};
};
} // namespace onda::plugin
