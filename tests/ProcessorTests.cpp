#include "Processor.h"
#include "RunViewHost.h"
#include "TestAudioFile.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace allocation_audit {
thread_local bool enabled{};
std::atomic<std::uint64_t> count{};

void record() noexcept {
  if (enabled)
    count.fetch_add(1, std::memory_order_relaxed);
}
} // namespace allocation_audit

namespace lock_audit {
thread_local bool enabled{};
std::atomic<std::uint64_t> count{};
} // namespace lock_audit

#if defined(__linux__)
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (lock_audit::enabled)
    lock_audit::count.fetch_add(1, std::memory_order_relaxed);
  return __real_pthread_mutex_lock(mutex);
}
#endif

void *operator new(const std::size_t size) {
  allocation_audit::record();
  if (auto *memory = std::malloc(std::max(size, std::size_t{1})))
    return memory;
  throw std::bad_alloc{};
}

void *operator new[](const std::size_t size) { return ::operator new(size); }

void *operator new(const std::size_t size, const std::align_val_t alignment) {
  allocation_audit::record();
  const auto align = static_cast<std::size_t>(alignment);
  const auto requested = std::max(size, std::size_t{1});
  const auto padded = ((requested + align - 1U) / align) * align;
  if (auto *memory = std::aligned_alloc(align, padded))
    return memory;
  throw std::bad_alloc{};
}

void *operator new[](const std::size_t size, const std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void *operator new(const std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](const std::size_t size,
                     const std::nothrow_t &tag) noexcept {
  return ::operator new(size, tag);
}

void *operator new(const std::size_t size, const std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](const std::size_t size, const std::align_val_t alignment,
                     const std::nothrow_t &tag) noexcept {
  return ::operator new(size, alignment, tag);
}

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}
void operator delete(void *memory, std::align_val_t) noexcept {
  std::free(memory);
}
void operator delete[](void *memory, std::align_val_t) noexcept {
  std::free(memory);
}
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
  std::free(memory);
}
void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept {
  std::free(memory);
}
void operator delete(void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
void operator delete(void *memory, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  std::free(memory);
}
void operator delete[](void *memory, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  std::free(memory);
}

namespace {

class TemporarySource final {
public:
  TemporarySource() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("onda-processor-fault-" + std::to_string(stamp) + ".onda");
  }

  ~TemporarySource() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  bool write(std::string_view source) const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << source;
    return output.good();
  }

  void remove() const {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class TemporaryProject final {
public:
  TemporaryProject() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = std::filesystem::temp_directory_path() /
                 ("onda-processor-project-" + std::to_string(stamp));
    std::filesystem::create_directories(directory_);
    entry_ = directory_ / "main.onda";
    dependency_ = directory_ / "dependency.onda";
  }

  ~TemporaryProject() {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  bool write(const std::string_view value) const {
    std::ofstream dependency(dependency_, std::ios::binary | std::ios::trunc);
    dependency << "def snapshot_value() { return " << value << " }\n";
    std::ofstream entry(entry_, std::ios::binary | std::ios::trunc);
    entry << "import dependency\n"
             "outs { out1, out2 }\n"
             "sample {\n"
             "  out1 = snapshot_value()\n"
             "  out2 = snapshot_value()\n"
             "}\n";
    return dependency.good() && entry.good();
  }

  void removeDependency() const {
    std::error_code ignored;
    std::filesystem::remove(dependency_, ignored);
  }

  void removeAllSources() const {
    std::error_code ignored;
    std::filesystem::remove(entry_, ignored);
    std::filesystem::remove(dependency_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &entry() const noexcept {
    return entry_;
  }

private:
  std::filesystem::path directory_;
  std::filesystem::path entry_;
  std::filesystem::path dependency_;
};

class TestPlayHead final : public juce::AudioPlayHead {
public:
  [[nodiscard]] juce::Optional<PositionInfo> getPosition() const override {
    ++positionRequests_;
    return position_;
  }

  void setPosition(juce::Optional<PositionInfo> position) {
    position_ = std::move(position);
  }

  [[nodiscard]] int positionRequests() const noexcept {
    return positionRequests_;
  }

private:
  juce::Optional<PositionInfo> position_;
  mutable int positionRequests_{};
};

constexpr auto instrumentEvents = R"(
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}
)";

std::string validSource(const onda::plugin::Product product) {
  if (product == onda::plugin::Product::instrument) {
    return std::string{"outs { out1, out2 }\n"} + instrumentEvents + R"(
sample {
  out1 = 0.25
  out2 = 0.25
}
)";
  }
  return R"(
ins { in1, in2 }
outs { out1, out2 }
sample {
  out1 = in1 * 0.25
  out2 = in2 * 0.25
}
)";
}

std::string faultingSource(const onda::plugin::Product product) {
  const auto interface =
      product == onda::plugin::Product::instrument
          ? std::string{"outs { out1, out2 }\n"} + instrumentEvents
          : std::string{"ins { in1, in2 }\nouts { out1, out2 }\n"};
  return interface + R"(
params { divisor: i32 = 0 }
def quotient(value: i32, by: i32) { return value / by }
sample {
  out1 = f32(quotient(i32(1), divisor))
  out2 = f32(quotient(i32(1), divisor))
}
)";
}

constexpr auto eventFaultingInstrumentSource = R"(
outs { out1, out2 }
init {
  held = 0.25
}
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {
  held = f32(i32(1) / i32(0))
}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}
sample {
  out1 = held
  out2 = held
}
)";

constexpr auto initFaultingEffectSource = R"(
ins { in1, in2 }
outs { out1, out2 }
init {
  initial = i32(1) / i32(0)
}
sample {
  out1 = in1 * f32(initial)
  out2 = in2 * f32(initial)
}
)";

constexpr auto parameterEffectSource = R"(
ins { in1, in2 }
outs { out1, out2 }
params {
  gain = 0.5 { 0.0, 1.0 }
}
sample {
  out1 = in1 * gain
  out2 = in2 * gain
}
)";

constexpr auto resetEffectSource = R"(
ins { in1, in2 }
outs { out1, out2 }
params {
  gain = 0.25 { 0.0, 1.0 }
}
init {
  multiplier = 1.0
}
event cc(channel: i32, index: i32, value: f32) {
  multiplier = value
}
sample {
  out1 = in1 * gain * multiplier
  out2 = in2 * gain * multiplier
}
)";

constexpr auto hostContextEffectSource = R"(
ins { in1, in2 }
outs { out1, out2 }
init {
  transport_value = f64(0.0)
  sample_value = f64(0.0)
  time_value = f64(0.0)
  tempo_value = f64(0.0)
  musical_value = f64(0.0)
  bar_value = f64(0.0)
  signature_value = f64(0.0)
  loop_value = f64(0.0)
  render_value = f64(0.0)
  midi_value = f64(0.0)
}
event transport(playing: bool, recording: bool, looping: bool) {
  transport_value = f64(0.0)
  if playing { transport_value = transport_value + 0.01 }
  if recording { transport_value = transport_value + 0.02 }
  if looping { transport_value = transport_value + 0.04 }
}
event sample_position(sample: i64) {
  sample_value = f64(sample) / 1000.0
}
event time_position(seconds: f64) {
  time_value = seconds / 100.0
}
event tempo(bpm: f64) {
  tempo_value = bpm / 1000.0
}
event musical_position(quarter_note: f64) {
  musical_value = quarter_note / 1000.0
}
event bar_position(start_quarter_note: f64) {
  bar_value = start_quarter_note / 1000.0
}
event time_signature(numerator: i32, denominator: i32) {
  signature_value = f64(numerator) / 1000.0 + f64(denominator) / 10000.0
}
event loop_region(start_quarter_note: f64, end_quarter_note: f64) {
  loop_value = start_quarter_note / 1000.0 + end_quarter_note / 10000.0
}
event render_mode(realtime: bool) {
  if realtime {
    render_value = f64(0.5)
  } else {
    render_value = f64(0.25)
  }
}
event cc(channel: i32, index: i32, value: f32) {
  midi_value = f64(value)
}
sample {
  out1 = f32(transport_value + sample_value + time_value + tempo_value + musical_value + bar_value + signature_value + loop_value + render_value + midi_value)
  out2 = f32(sample_value)
}
)";

constexpr auto extendedMidiInstrumentSource = R"(
outs { out1, out2 }
init { held = 0.0 }
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}
event poly_pressure(channel: i32, key: i32, pressure: f32) {
  held = pressure
}
event program_change(channel: i32, program: i32) {
  held = f32(program) / 127.0
}
sample {
  out1 = held
  out2 = held
}
)";

constexpr std::uint32_t stateMagic = 0x41444e4fU;
constexpr int stateVersion = 2;

bool waitForOutput(onda::plugin::Processor &processor, const float expected,
                   const int frames = 64) {
  juce::AudioBuffer<float> audio(2, frames);
  juce::MidiBuffer midi;
  for (int attempt = 0; attempt < 500; ++attempt) {
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
      std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(), 1.0F);
    processor.processBlock(audio, midi);

    auto matches = true;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
      for (int frame = 0; frame < audio.getNumSamples(); ++frame)
        matches &=
            std::abs(audio.getSample(channel, frame) - expected) < 1.0e-5F;
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    if (matches)
      return true;
  }
  return false;
}

bool waitForActiveRevision(onda::plugin::Processor &processor,
                           const std::uint64_t previousRevision) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    const auto status = processor.workerStatus();
    if (status.revision > previousRevision && status.active &&
        !status.compiling && status.message == "Active") {
      return true;
    }
  }
  return false;
}

bool waitForPublishedReplacement(onda::plugin::Processor &processor) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    const auto status = processor.workerStatus();
    if (status.active && !status.compiling && status.message == "Active")
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool waitForSafeEffectBypass(onda::plugin::Processor &processor) {
  juce::AudioBuffer<float> audio(2, 64);
  juce::MidiBuffer midi;
  for (int attempt = 0; attempt < 500; ++attempt) {
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
      std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(), 1.0F);
    processor.processBlock(audio, midi);
    const auto dry = std::all_of(
        audio.getArrayOfReadPointers(),
        audio.getArrayOfReadPointers() + audio.getNumChannels(),
        [&audio](const float *channel) {
          return std::all_of(channel, channel + audio.getNumSamples(),
                             [](const float sample) {
                               return std::abs(sample - 1.0F) < 1.0e-6F;
                             });
        });
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    const auto status = processor.workerStatus();
    if (dry && !status.active && !status.compiling &&
        status.message != "Waiting to compile" &&
        status.message != "Waiting for host specialization") {
      return true;
    }
  }
  return false;
}

bool waitForProjectImageOutput(onda::plugin::Processor &processor,
                               const float expected, const int frames = 64) {
  if (!waitForOutput(processor, expected, frames))
    return false;
  const auto status = processor.workerStatus();
  return status.active && !status.compiling && status.usingProjectImage &&
         status.message.starts_with("Active from saved project:");
}

juce::MemoryBlock
makeState(const juce::String &path, const juce::String &browseDirectory,
          const std::array<float, onda::plugin::slotCount> &values,
          const int width, const int height,
          const std::span<const std::pair<juce::String, juce::String>>
              bufferBindings = {}) {
  juce::MemoryBlock state;
  juce::MemoryOutputStream stream(state, false);
  stream.writeInt(static_cast<int>(stateMagic));
  stream.writeInt(stateVersion);
  stream.writeString(path);
  stream.writeString(browseDirectory);
  stream.writeInt(static_cast<int>(bufferBindings.size()));
  for (const auto &[name, bufferPath] : bufferBindings) {
    stream.writeString(name);
    stream.writeString(bufferPath);
  }
  stream.writeInt64(0);
  for (const auto value : values)
    stream.writeFloat(value);
  stream.writeInt(width);
  stream.writeInt(height);
  return state;
}

bool processWithoutAllocation(onda::plugin::Processor &processor,
                              const float expected) {
  juce::AudioBuffer<float> audio(2, 64);
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), 16);
  midi.addEvent(juce::MidiMessage::controllerEvent(1, 74, 96), 32);
  for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(), 1.0F);

  allocation_audit::count.store(0, std::memory_order_relaxed);
  lock_audit::count.store(0, std::memory_order_relaxed);
  allocation_audit::enabled = true;
  lock_audit::enabled = true;
  processor.processBlock(audio, midi);
  lock_audit::enabled = false;
  allocation_audit::enabled = false;
  if (allocation_audit::count.load(std::memory_order_relaxed) != 0) {
    std::cerr << "audio callback allocated memory\n";
    return false;
  }
  if (lock_audit::count.load(std::memory_order_relaxed) != 0) {
    std::cerr << "audio callback acquired a mutex\n";
    return false;
  }
  for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
    for (int frame = 0; frame < audio.getNumSamples(); ++frame) {
      if (std::abs(audio.getSample(channel, frame) - expected) >= 1.0e-5F)
        return false;
    }
  }
  return true;
}

bool exerciseRunViewAdapter() {
  const auto root = onda::plugin::runViewResource("/");
  const auto index = onda::plugin::runViewResource("/index.html?cache=1");
  const auto script = onda::plugin::runViewResource("/param-control.js?v=2");
  if (!root || !index || !script || root->data.size() <= 3U ||
      index->data != root->data ||
      root->mimeType != "text/html; charset=utf-8" ||
      script->mimeType != "text/javascript; charset=utf-8" ||
      root->data[0] != std::byte{0xef} || root->data[1] != std::byte{0xbb} ||
      root->data[2] != std::byte{0xbf} ||
      onda::plugin::runViewResource("/missing.txt")) {
    std::cerr << "embedded run-view resources were not served canonically\n";
    return false;
  }

  onda::plugin::Processor processor(onda::plugin::Product::effect);
  const auto message = onda::plugin::makeRunViewState(
      processor, processor.workerStatus(), processor.canExportProject(),
      "adapter test error");
  const auto *envelope = message.getDynamicObject();
  if (envelope == nullptr ||
      envelope->getProperty("type").toString() != "state") {
    std::cerr << "run-view state envelope was invalid\n";
    return false;
  }
  const auto stateValue = envelope->getProperty("state");
  const auto *state = stateValue.getDynamicObject();
  if (state == nullptr ||
      !static_cast<bool>(state->getProperty("supportsSourceSelection")) ||
      !static_cast<bool>(state->getProperty("supportsProjectExport")) ||
      static_cast<bool>(state->getProperty("supportsBufferEmbedding")) ||
      static_cast<bool>(state->getProperty("supportsTransport")) ||
      static_cast<bool>(state->getProperty("supportsDeviceSelection")) ||
      static_cast<bool>(state->getProperty("supportsRunSettings")) ||
      !static_cast<bool>(state->getProperty("supportsReset")) ||
      static_cast<bool>(state->getProperty("supportsScope")) ||
      static_cast<bool>(state->getProperty("canExportProject")) ||
      state->getProperty("error").toString() != "adapter test error") {
    std::cerr << "run-view capability state was invalid\n";
    return false;
  }
  return true;
}

bool exercise(const onda::plugin::Product product) {
  TemporarySource source;
  if (!source.write(validSource(product)))
    return false;

  onda::plugin::Processor processor(product);
  if (processor.getNumPrograms() != 1 ||
      processor.getProgramName(0) != "Default") {
    std::cerr << "processor must expose a named default factory program\n";
    return false;
  }
  processor.prepareToPlay(48'000.0, 64);
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForOutput(processor, 0.25F)) {
    std::cerr << "valid processor did not become active\n";
    return false;
  }
  if (!processWithoutAllocation(processor, 0.25F))
    return false;

  for (const auto frames : {1, 7, 64}) {
    if (!waitForOutput(processor, 0.25F, frames)) {
      std::cerr << "varying callback size failed at " << frames << " frames\n";
      return false;
    }
  }
  if (!waitForOutput(processor, 0.25F, 96)) {
    std::cerr << "oversized callback did not trigger a safe specialization\n";
    return false;
  }

  if (product == onda::plugin::Product::effect) {
    const auto revisionBeforeInitFailure = processor.workerStatus().revision;
    if (!source.write(initFaultingEffectSource))
      return false;
    bool retainedLastGood = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
      if (!waitForOutput(processor, 0.25F))
        return false;
      const auto status = processor.workerStatus();
      if (status.revision > revisionBeforeInitFailure && status.active &&
          !status.compiling && status.message != "Active") {
        retainedLastGood = true;
        break;
      }
    }
    if (!retainedLastGood) {
      std::cerr << "init-time failure did not retain the last-good engine\n";
      return false;
    }
    const auto failedRevision = processor.workerStatus().revision;
    if (!source.write(validSource(product)) ||
        !waitForActiveRevision(processor, failedRevision) ||
        !waitForOutput(processor, 0.25F)) {
      std::cerr << "processor did not recover after init-time failure\n";
      return false;
    }
  }

  if (product == onda::plugin::Product::instrument) {
    const auto previousRevision = processor.workerStatus().revision;
    if (!source.write(eventFaultingInstrumentSource) ||
        !waitForActiveRevision(processor, previousRevision) ||
        !waitForOutput(processor, 0.25F)) {
      std::cerr << "event-faulting instrument did not become active\n";
      return false;
    }
    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), 0);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
      std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(), 1.0F);
    processor.processBlock(audio, midi);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
      for (int frame = 0; frame < audio.getNumSamples(); ++frame) {
        if (std::abs(audio.getSample(channel, frame)) >= 1.0e-6F) {
          std::cerr << "event runtime failure did not silence the instrument\n";
          return false;
        }
      }
    }
    if (processor.workerStatus().message.find("runtime safety check") ==
        std::string::npos) {
      std::cerr << "event runtime failure was not reported\n";
      return false;
    }
    if (!source.write(validSource(product))) {
      return false;
    }
    processor.reset();
    if (!waitForOutput(processor, 0.25F)) {
      std::cerr << "instrument did not recover after event runtime failure\n";
      return false;
    }
  }

  if (!source.write(faultingSource(product)))
    return false;
  const auto fallback =
      product == onda::plugin::Product::instrument ? 0.0F : 1.0F;
  if (!waitForOutput(processor, fallback)) {
    std::cerr << "runtime failure did not activate product fallback\n";
    return false;
  }
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  const auto failed = processor.workerStatus();
  if (failed.active ||
      failed.message.find("runtime safety check") == std::string::npos) {
    std::cerr << "runtime failure was not reported to the editor state\n";
    return false;
  }

  if (!source.write(validSource(product)))
    return false;
  processor.reset();
  if (!waitForOutput(processor, 0.25F)) {
    std::cerr << "Reset did not recover with valid source\n";
    return false;
  }
  const auto recovered = processor.workerStatus();
  if (!recovered.active || recovered.message != "Active") {
    std::cerr << "recovered engine retained stale runtime-failure status\n";
    return false;
  }
  if (product == onda::plugin::Product::effect) {
    if (!source.write(faultingSource(product)) ||
        !waitForOutput(processor, 1.0F)) {
      return false;
    }
    processor.unload();
    const auto unloaded = processor.workerStatus();
    if (unloaded.active || unloaded.message != "No Onda file loaded") {
      std::cerr << "unload retained stale runtime-failure state\n";
      return false;
    }
  }
  processor.releaseResources();
  return true;
}

bool exerciseStateRestore() {
  TemporarySource source;
  if (!source.write(parameterEffectSource))
    return false;

  juce::MemoryBlock state;
  {
    onda::plugin::Processor original(onda::plugin::Product::effect);
    original.prepareToPlay(48'000.0, 64);
    original.loadFile(juce::File(source.path().string()), false);
    if (!waitForOutput(original, 0.5F))
      return false;
    original.setSlotValue(0, 0.8125F);
    if (!waitForOutput(original, 0.8125F)) {
      std::cerr << "host automation did not reach DSP without an editor\n";
      return false;
    }
    const auto revisionBeforeReload = original.workerStatus().revision;
    if (!source.write(std::string{parameterEffectSource} + "\n# reload\n"))
      return false;
    original.requestReload();
    bool reloaded = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
      const auto status = original.workerStatus();
      if (status.revision > revisionBeforeReload && status.active &&
          !status.compiling && status.message == "Active") {
        reloaded = true;
        break;
      }
    }
    if (!reloaded || !waitForOutput(original, 0.8125F)) {
      std::cerr << "host slot value was not preserved across reload\n";
      return false;
    }
    original.setEditorSize(777, 888);
    original.getStateInformation(state);

    const auto unchanged = [&] {
      const auto [width, height] = original.editorSize();
      return std::abs(original.slotValue(0) - 0.8125F) < 1.0e-6F &&
             width == 777 && height == 888 &&
             original.workerStatus().path == source.path();
    };
    const auto reject = [&](const juce::MemoryBlock &candidate) {
      original.setStateInformation(candidate.getData(),
                                   static_cast<int>(candidate.getSize()));
      return unchanged();
    };

    juce::MemoryBlock wrongMagic;
    juce::MemoryOutputStream wrongMagicStream(wrongMagic, false);
    wrongMagicStream.writeInt(0);
    wrongMagicStream.writeInt(stateVersion);
    juce::MemoryBlock wrongVersion;
    juce::MemoryOutputStream wrongVersionStream(wrongVersion, false);
    wrongVersionStream.writeInt(static_cast<int>(stateMagic));
    wrongVersionStream.writeInt(stateVersion + 1);
    const juce::MemoryBlock truncated(state.getData(), state.getSize() - 1U);
    std::array<float, onda::plugin::slotCount> values{};
    values.fill(0.5F);
    const auto longPath = makeState(
        juce::String(std::string(16U * 1024U + 1U, 'x')), {}, values, 480, 720);
    juce::MemoryBlock excessiveBindingCount;
    juce::MemoryOutputStream excessiveBindingStream(excessiveBindingCount,
                                                    false);
    excessiveBindingStream.writeInt(static_cast<int>(stateMagic));
    excessiveBindingStream.writeInt(stateVersion);
    excessiveBindingStream.writeString({});
    excessiveBindingStream.writeString({});
    excessiveBindingStream.writeInt(1025);
    const std::array longNameBinding{
        std::pair{juce::String(std::string(1025U, 'x')),
                  juce::String{"/tmp/audio.flac"}}};
    const auto longBufferName =
        makeState({}, {}, values, 480, 720, longNameBinding);
    const std::array emptyNameBinding{
        std::pair{juce::String{}, juce::String{"/tmp/audio.flac"}}};
    const auto emptyBufferName =
        makeState({}, {}, values, 480, 720, emptyNameBinding);
    juce::MemoryBlock unterminatedPath;
    juce::MemoryOutputStream unterminatedPathStream(unterminatedPath, false);
    unterminatedPathStream.writeInt(static_cast<int>(stateMagic));
    unterminatedPathStream.writeInt(stateVersion);
    const std::string nonTerminated(16U * 1024U + 1U, 'x');
    unterminatedPathStream.write(nonTerminated.data(), nonTerminated.size());
    juce::MemoryBlock invalidUtf8;
    juce::MemoryOutputStream invalidUtf8Stream(invalidUtf8, false);
    invalidUtf8Stream.writeInt(static_cast<int>(stateMagic));
    invalidUtf8Stream.writeInt(stateVersion);
    const std::array invalidUtf8Path{static_cast<char>(0xc0),
                                     static_cast<char>(0x80), '\0'};
    invalidUtf8Stream.write(invalidUtf8Path.data(), invalidUtf8Path.size());
    juce::MemoryBlock oversized(8U * 1024U * 1024U + 1U, true);
    if (!reject(wrongMagic) || !reject(wrongVersion) || !reject(truncated) ||
        !reject(longPath) || !reject(excessiveBindingCount) ||
        !reject(longBufferName) || !reject(emptyBufferName) ||
        !reject(unterminatedPath) || !reject(invalidUtf8) ||
        !reject(oversized)) {
      std::cerr << "invalid state payload mutated processor state\n";
      return false;
    }

    TemporarySource invalidSource;
    if (!invalidSource.write("this is not valid Onda source\n"))
      return false;
    std::array<float, onda::plugin::slotCount> invalidValues{};
    invalidValues.fill(0.25F);
    const auto invalidState =
        makeState(juce::String(invalidSource.path().string()), {},
                  invalidValues, 480, 720);
    original.setStateInformation(invalidState.getData(),
                                 static_cast<int>(invalidState.getSize()));
    if (!waitForSafeEffectBypass(original)) {
      std::cerr
          << "invalid state restore retained the previously active engine\n";
      return false;
    }
    original.setStateInformation(state.getData(),
                                 static_cast<int>(state.getSize()));
    if (!waitForOutput(original, 0.8125F)) {
      std::cerr << "processor did not recover after an invalid state restore\n";
      return false;
    }
  }

  {
    onda::plugin::Processor restored(onda::plugin::Product::effect);
    restored.setStateInformation(state.getData(),
                                 static_cast<int>(state.getSize()));
    juce::MemoryBlock beforePrepare;
    restored.getStateInformation(beforePrepare);
    if (beforePrepare.getSize() != state.getSize() ||
        std::memcmp(beforePrepare.getData(), state.getData(),
                    state.getSize()) != 0) {
      std::cerr << "restored state was not stable before host preparation\n";
      return false;
    }
    restored.prepareToPlay(44'100.0, 32);
    if (!waitForOutput(restored, 0.8125F, 32)) {
      std::cerr << "loaded path did not restore after host configuration\n";
      return false;
    }
    const auto [width, height] = restored.editorSize();
    if (std::abs(restored.slotValue(0) - 0.8125F) >= 1.0e-6F || width != 777 ||
        height != 888 || restored.workerStatus().path != source.path()) {
      std::cerr << "path, slot, or editor state did not restore\n";
      return false;
    }
    juce::MemoryBlock roundTrip;
    restored.getStateInformation(roundTrip);
    if (roundTrip.getSize() != state.getSize() ||
        std::memcmp(roundTrip.getData(), state.getData(), state.getSize()) !=
            0) {
      std::cerr << "repeated state save/restore was not stable\n";
      return false;
    }
  }

  {
    std::array<float, onda::plugin::slotCount> hostileValues{};
    hostileValues.fill(0.25F);
    hostileValues[0] = std::numeric_limits<float>::quiet_NaN();
    hostileValues[1] = std::numeric_limits<float>::infinity();
    hostileValues[2] = -1.0F;
    hostileValues[3] = 2.0F;
    const auto hostileState =
        makeState({}, juce::String::fromUTF8("/tmp/Ønda/状態"), hostileValues,
                  -100, 100'000);
    onda::plugin::Processor normalized(onda::plugin::Product::effect);
    normalized.setStateInformation(hostileState.getData(),
                                   static_cast<int>(hostileState.getSize()));
    const auto [width, height] = normalized.editorSize();
    if (std::abs(normalized.slotValue(0) - 0.5F) >= 1.0e-6F ||
        std::abs(normalized.slotValue(1) - 0.5F) >= 1.0e-6F ||
        std::abs(normalized.slotValue(2)) >= 1.0e-6F ||
        std::abs(normalized.slotValue(3) - 1.0F) >= 1.0e-6F || width != 360 ||
        height != 1400 ||
        normalized.lastBrowseDirectory().getFullPathName() !=
            juce::String::fromUTF8("/tmp/Ønda/状態")) {
      std::cerr << "hostile finite, non-finite, or Unicode state was not "
                   "normalized safely\n";
      return false;
    }
  }

  if (!source.write("this is not valid Onda source\n"))
    return false;
  {
    onda::plugin::Processor invalid(onda::plugin::Product::effect);
    invalid.setStateInformation(state.getData(),
                                static_cast<int>(state.getSize()));
    invalid.prepareToPlay(48'000.0, 64);
    if (!waitForProjectImageOutput(invalid, 0.8125F)) {
      std::cerr << "invalid restored source did not use the saved project\n";
      return false;
    }
  }

  source.remove();
  onda::plugin::Processor missing(onda::plugin::Product::effect);
  missing.setStateInformation(state.getData(),
                              static_cast<int>(state.getSize()));
  missing.prepareToPlay(48'000.0, 64);
  if (!waitForProjectImageOutput(missing, 0.8125F)) {
    std::cerr << "missing restored path did not use the saved project\n";
    return false;
  }
  return true;
}

bool exerciseEditorLifecycle() {
  TemporarySource source;
  if (!source.write(parameterEffectSource))
    return false;

  onda::plugin::Processor processor(onda::plugin::Product::effect);
  processor.prepareToPlay(48'000.0, 64);
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForOutput(processor, 0.5F) ||
      !waitForPublishedReplacement(processor)) {
    return false;
  }
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  const auto activeRevision = processor.workerStatus().revision;
  auto *const parameter = processor.getParameters()[0];

  const auto automate = [&](const float value, const char *phase) {
    parameter->setValueNotifyingHost(value);
    const auto reachedDsp = waitForOutput(processor, value);
    const auto slot = processor.slotValue(0);
    const auto revision = processor.workerStatus().revision;
    if (!reachedDsp || std::abs(slot - value) >= 1.0e-6F ||
        revision != activeRevision) {
      std::cerr << "host automation failed " << phase << " (DSP " << reachedDsp
                << ", slot " << slot << ", revision " << revision
                << ", expected revision " << activeRevision << ")\n";
      return false;
    }
    return true;
  };

  if (processor.getActiveEditor() != nullptr ||
      !automate(0.2F, "before editor creation")) {
    return false;
  }

  std::unique_ptr<juce::AudioProcessorEditor> editor{
      processor.createEditorAndMakeActive()};
  if (!editor || processor.getActiveEditor() != editor.get()) {
    std::cerr << "processor did not create and retain its editor\n";
    return false;
  }
  editor->setVisible(true);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  if (!automate(0.4F, "with the editor open"))
    return false;

  editor->setVisible(false);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  if (!automate(0.6F, "with the editor unavailable"))
    return false;

  processor.editorBeingDeleted(editor.get());
  editor.reset();
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  if (processor.getActiveEditor() != nullptr ||
      !automate(0.8F, "after editor destruction")) {
    return false;
  }

  processor.releaseResources();
  return true;
}

bool exerciseAudioFileBuffers() {
  TemporarySource source;
  if (!source.write(R"(
buffers {
  clip: buffer<f32[2]>
}
outs { out1, out2 }
init {
  index = 0
}
sample {
  out1 = clip[0, index]
  out2 = clip[1, index]
  index = (index + 1) % 8
}
)")) {
    return false;
  }

  TemporaryAudioFile audioFile{"onda-plugin-buffer-processor-test"};
  if (!audioFile.write(0.1F, 0.5F))
    return false;

  const auto waitForBufferStatus = [](onda::plugin::Processor &processor,
                                      const bool active) {
    for (int attempt = 0; attempt < 500; ++attempt) {
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
      const auto status = processor.workerStatus();
      if (!status.compiling && status.buffers.size() == 1U &&
          status.active == active) {
        return status;
      }
    }
    return onda::plugin::WorkerStatus{};
  };
  const auto processFirstFrame = [](onda::plugin::Processor &processor) {
    juce::AudioBuffer<float> audio(2, 8);
    juce::MidiBuffer midi;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
      std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(), 1.0F);
    processor.processBlock(audio, midi);
    return std::pair{audio.getSample(0, 0), audio.getSample(1, 0)};
  };

  juce::MemoryBlock state;
  juce::MemoryBlock portableState;
  {
    onda::plugin::Processor processor(onda::plugin::Product::effect);
    processor.prepareToPlay(48'000.0, 8);
    processor.loadFile(juce::File(source.path().string()), false);
    const auto unbound = waitForBufferStatus(processor, false);
    if (unbound.buffers.size() != 1U ||
        unbound.message.find("not bound") == std::string::npos) {
      std::cerr << "declared buffer did not wait for an audio-file binding\n";
      return false;
    }

    processor.bindBufferFile("clip", juce::File(audioFile.path().string()));
    const auto bound = waitForBufferStatus(processor, true);
    if (bound.buffers.size() != 1U ||
        bound.buffers[0].loadedPath != audioFile.path() ||
        bound.buffers[0].loadedFrames != 8 ||
        bound.buffers[0].loadedChannels != 2) {
      std::cerr << "processor did not publish decoded buffer metadata\n";
      return false;
    }
    const auto [left, right] = processFirstFrame(processor);
    if (std::abs(left - 0.1F) >= 5.0e-4F || std::abs(right - 0.5F) >= 5.0e-4F) {
      std::cerr << "processor did not render the bound FLAC file\n";
      return false;
    }
    processor.getStateInformation(state);

    if (!processor.canExportProject()) {
      std::cerr << "portable project image did not become ready\n";
      return false;
    }
    processor.getStateInformation(portableState);

    const auto revision = processor.workerStatus().revision;
    if (!audioFile.write(0.2F, 0.6F))
      return false;
    auto reloaded = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
      const auto status = processor.workerStatus();
      const auto output = processFirstFrame(processor);
      if (status.revision > revision && status.active && !status.compiling &&
          std::abs(output.first - 0.2F) < 5.0e-4F &&
          std::abs(output.second - 0.6F) < 5.0e-4F) {
        reloaded = true;
        break;
      }
    }
    if (!reloaded) {
      std::cerr << "audio-file change did not trigger an engine replacement\n";
      return false;
    }

    processor.clearBuffer("clip");
    const auto fallback = processFirstFrame(processor);
    if (std::abs(fallback.first - 1.0F) >= 1.0e-6F ||
        std::abs(fallback.second - 1.0F) >= 1.0e-6F) {
      std::cerr << "clearing a buffer did not deactivate the engine\n";
      return false;
    }
    const auto cleared = waitForBufferStatus(processor, false);
    if (cleared.buffers.size() != 1U ||
        !cleared.buffers[0].loadedPath.empty()) {
      std::cerr << "cleared buffer remained bound in worker status\n";
      return false;
    }
  }

  {
    onda::plugin::Processor restored(onda::plugin::Product::effect);
    restored.setStateInformation(state.getData(),
                                 static_cast<int>(state.getSize()));
    restored.prepareToPlay(48'000.0, 8);
    const auto status = waitForBufferStatus(restored, true);
    const auto output = processFirstFrame(restored);
    if (status.buffers.size() != 1U ||
        status.buffers[0].loadedPath != audioFile.path() ||
        std::abs(output.first - 0.2F) >= 5.0e-4F ||
        std::abs(output.second - 0.6F) >= 5.0e-4F) {
      std::cerr << "buffer path did not restore from plugin state\n";
      return false;
    }
  }

  juce::MemoryBlock refreshedPortableState;
  {
    onda::plugin::Processor restored(onda::plugin::Product::effect);
    restored.setStateInformation(portableState.getData(),
                                 static_cast<int>(portableState.getSize()));
    restored.prepareToPlay(48'000.0, 8);
    const auto status = waitForBufferStatus(restored, true);
    const auto output = processFirstFrame(restored);
    if (status.usingProjectImage || std::abs(output.first - 0.2F) >= 5.0e-4F ||
        std::abs(output.second - 0.6F) >= 5.0e-4F) {
      std::cerr << "valid disk buffers did not override embedded state\n";
      return false;
    }
    restored.getStateInformation(refreshedPortableState);
  }

  source.remove();
  {
    std::error_code ignored;
    std::filesystem::remove(audioFile.path(), ignored);
  }
  {
    onda::plugin::Processor restored(onda::plugin::Product::effect);
    restored.setStateInformation(portableState.getData(),
                                 static_cast<int>(portableState.getSize()));
    restored.prepareToPlay(48'000.0, 8);
    const auto status = waitForBufferStatus(restored, true);
    const auto output = processFirstFrame(restored);
    if (!status.usingProjectImage || std::abs(output.first - 0.1F) >= 5.0e-4F ||
        std::abs(output.second - 0.5F) >= 5.0e-4F) {
      std::cerr
          << "portable state did not restore source and buffer checkpoint\n";
      return false;
    }

    const auto viewMessage = onda::plugin::makeRunViewState(
        restored, restored.workerStatus(), restored.canExportProject(), {});
    const auto *viewEnvelope = viewMessage.getDynamicObject();
    const auto viewStateValue = viewEnvelope == nullptr
                                    ? juce::var{}
                                    : viewEnvelope->getProperty("state");
    const auto *viewState = viewStateValue.getDynamicObject();
    const auto viewBuffersValue =
        viewState == nullptr ? juce::var{} : viewState->getProperty("buffers");
    const auto *viewBuffers = viewBuffersValue.getArray();
    const auto *viewBuffer =
        viewBuffers == nullptr || viewBuffers->size() != 1
            ? nullptr
            : viewBuffers->getReference(0).getDynamicObject();
    if (viewState == nullptr || viewBuffer == nullptr ||
        !static_cast<bool>(viewState->getProperty("canExportProject")) ||
        viewBuffer->getProperty("loadedPath").toString().isNotEmpty() ||
        static_cast<int>(viewBuffer->getProperty("loadedFrames")) != 8 ||
        static_cast<int>(viewBuffer->getProperty("loadedChannels")) != 2 ||
        std::abs(
            static_cast<double>(viewBuffer->getProperty("loadedSampleRate")) -
            48'000.0) >= 1.0e-6) {
      std::cerr << "run view did not expose embedded project-buffer metadata\n";
      return false;
    }

    const auto exportDirectory =
        std::filesystem::temp_directory_path() /
        ("onda-portable-buffer-project-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto saveError =
        restored.saveProjectAs(juce::File(exportDirectory.string()));
    auto relinked = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
      const auto relinkedStatus = restored.workerStatus();
      if (relinkedStatus.active && !relinkedStatus.compiling &&
          !relinkedStatus.usingProjectImage &&
          relinkedStatus.path == exportDirectory / "project.ondaproject" &&
          relinkedStatus.buffers.size() == 1U &&
          relinkedStatus.buffers[0].loadedPath.empty() &&
          relinkedStatus.buffers[0].loadedFrames == 8 &&
          relinkedStatus.buffers[0].loadedChannels == 2) {
        relinked = true;
        break;
      }
    }
    const auto relinkedOutput = processFirstFrame(restored);
    std::error_code ignored;
    std::filesystem::remove_all(exportDirectory, ignored);
    if (!saveError.empty() || !relinked ||
        std::abs(relinkedOutput.first - 0.1F) >= 5.0e-4F ||
        std::abs(relinkedOutput.second - 0.5F) >= 5.0e-4F) {
      std::cerr << "Save Project As did not materialize and relink buffers: "
                << saveError << '\n';
      return false;
    }
  }
  {
    onda::plugin::Processor restored(onda::plugin::Product::effect);
    restored.setStateInformation(
        refreshedPortableState.getData(),
        static_cast<int>(refreshedPortableState.getSize()));
    restored.prepareToPlay(48'000.0, 8);
    const auto status = waitForBufferStatus(restored, true);
    const auto output = processFirstFrame(restored);
    if (!status.usingProjectImage || std::abs(output.first - 0.2F) >= 5.0e-4F ||
        std::abs(output.second - 0.6F) >= 5.0e-4F) {
      std::cerr << "disk buffer authority did not refresh the checkpoint\n";
      return false;
    }
  }
  return true;
}

bool exerciseSourceGraphFallbackAndDiskAuthority() {
  TemporaryProject project;
  if (!project.write("0.25"))
    return false;

  juce::MemoryBlock saved;
  {
    onda::plugin::Processor processor(onda::plugin::Product::effect);
    processor.prepareToPlay(48'000.0, 64);
    processor.loadFile(juce::File(project.entry().string()), false);
    if (!waitForOutput(processor, 0.25F))
      return false;
    processor.getStateInformation(saved);
  }

  project.removeDependency();
  {
    onda::plugin::Processor restored(onda::plugin::Product::effect);
    restored.setStateInformation(saved.getData(),
                                 static_cast<int>(saved.getSize()));
    restored.prepareToPlay(48'000.0, 64);
    if (!waitForProjectImageOutput(restored, 0.25F)) {
      std::cerr << "missing imported source did not restore from graph\n";
      return false;
    }
  }

  if (!project.write("0.75"))
    return false;
  juce::MemoryBlock refreshed;
  {
    onda::plugin::Processor linked(onda::plugin::Product::effect);
    linked.setStateInformation(saved.getData(),
                               static_cast<int>(saved.getSize()));
    linked.prepareToPlay(48'000.0, 64);
    if (!waitForOutput(linked, 0.75F) ||
        linked.workerStatus().usingProjectImage) {
      std::cerr
          << "valid linked disk project did not override the saved image\n";
      return false;
    }
    linked.getStateInformation(refreshed);
  }

  project.removeAllSources();
  onda::plugin::Processor refreshedRestore(onda::plugin::Product::effect);
  refreshedRestore.setStateInformation(refreshed.getData(),
                                       static_cast<int>(refreshed.getSize()));
  refreshedRestore.prepareToPlay(48'000.0, 64);
  if (!waitForProjectImageOutput(refreshedRestore, 0.75F)) {
    std::cerr << "disk authority did not become the new saved project\n";
    return false;
  }
  return true;
}

bool exerciseProjectExportRelinksAuthority() {
  TemporaryProject project;
  if (!project.write("0.25"))
    return false;

  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto exportRoot = std::filesystem::temp_directory_path() /
                          ("onda-processor-export-" + std::to_string(stamp));
  std::filesystem::create_directories(exportRoot);
  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(exportRoot, ignored);
  };

  onda::plugin::Processor processor(onda::plugin::Product::effect);
  processor.prepareToPlay(48'000.0, 64);
  processor.loadFile(juce::File(project.entry().string()), false);
  if (!waitForOutput(processor, 0.25F)) {
    cleanup();
    return false;
  }
  std::optional<std::string> saveError;
  if (!processor.saveProjectAsAsync(
          juce::File(exportRoot.string()),
          [&saveError](std::string error) { saveError = std::move(error); })) {
    std::cerr << "Save Project As could not start asynchronously\n";
    cleanup();
    return false;
  }
  for (int attempt = 0; attempt < 500 && !saveError; ++attempt)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  if (!saveError || !saveError->empty() || !waitForOutput(processor, 0.25F) ||
      processor.workerStatus().path != exportRoot / "project.ondaproject") {
    std::cerr << "Save Project As did not relink the exported disk project: "
              << (saveError ? *saveError : "timed out") << '\n';
    cleanup();
    return false;
  }

  project.removeAllSources();
  {
    std::ofstream dependency(exportRoot / "code/dependency.onda",
                             std::ios::binary | std::ios::trunc);
    dependency << "def snapshot_value() { return 0.875 }\n";
    if (!dependency) {
      cleanup();
      return false;
    }
  }
  if (!waitForOutput(processor, 0.875F)) {
    std::cerr << "exported project did not become the new disk authority\n";
    cleanup();
    return false;
  }
  cleanup();
  return true;
}

bool exerciseExtendedMidi() {
  TemporarySource source;
  if (!source.write(extendedMidiInstrumentSource))
    return false;

  onda::plugin::Processor processor(onda::plugin::Product::instrument);
  processor.prepareToPlay(48'000.0, 8);
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForPublishedReplacement(processor)) {
    std::cerr << "extended MIDI instrument did not become active\n";
    return false;
  }

  juce::AudioBuffer<float> audio(2, 8);
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::aftertouchChange(3, 60, 64), 2);
  midi.addEvent(juce::MidiMessage::programChange(3, 63), 5);
  processor.processBlock(audio, midi);
  for (int frame = 0; frame < audio.getNumSamples(); ++frame) {
    const auto expected = frame < 2   ? 0.0F
                          : frame < 5 ? 64.0F / 127.0F
                                      : 63.0F / 127.0F;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
      if (std::abs(audio.getSample(channel, frame) - expected) >= 1.0e-5F) {
        std::cerr << "poly pressure or program change scheduling failed\n";
        return false;
      }
    }
  }
  return true;
}

bool exerciseEventMetadataGating() {
  TemporarySource source;
  if (!source.write(validSource(onda::plugin::Product::effect)))
    return false;

  TestPlayHead playHead;
  juce::AudioPlayHead::PositionInfo position;
  position.setBpm(120.0);
  playHead.setPosition(position);

  onda::plugin::Processor processor(onda::plugin::Product::effect);
  processor.setPlayHead(&playHead);
  processor.prepareToPlay(48'000.0, 8);
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForPublishedReplacement(processor)) {
    std::cerr << "metadata-gating effect did not become active\n";
    return false;
  }

  juce::AudioBuffer<float> audio(2, 8);
  audio.clear();
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::programChange(1, 12), 4);
  processor.processBlock(audio, midi);
  if (playHead.positionRequests() != 0) {
    std::cerr << "engine without context declarations queried the playhead\n";
    return false;
  }
  return true;
}

bool exerciseHostContext() {
  TemporarySource source;
  if (!source.write(hostContextEffectSource))
    return false;

  TestPlayHead playHead;
  juce::AudioPlayHead::PositionInfo firstPosition;
  firstPosition.setTimeInSamples(100);
  firstPosition.setTimeInSeconds(2.0);
  firstPosition.setBpm(120.0);
  firstPosition.setPpqPosition(4.0);
  firstPosition.setPpqPositionOfLastBarStart(3.0);
  firstPosition.setTimeSignature(
      juce::AudioPlayHead::TimeSignature{.numerator = 3, .denominator = 4});
  firstPosition.setLoopPoints(
      juce::AudioPlayHead::LoopPoints{.ppqStart = 1.0, .ppqEnd = 8.0});
  firstPosition.setIsPlaying(true);
  firstPosition.setIsLooping(true);
  playHead.setPosition(firstPosition);

  onda::plugin::Processor processor(onda::plugin::Product::effect);
  processor.setPlayHead(&playHead);
  processor.prepareToPlay(48'000.0, 8);
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForPublishedReplacement(processor)) {
    std::cerr << "host-context effect did not become active: "
              << processor.workerStatus().message << '\n';
    return false;
  }

  juce::AudioBuffer<float> firstAudio(2, 6);
  juce::MidiBuffer emptyMidi;
  processor.processBlock(firstAudio, emptyMidi);
  if (playHead.positionRequests() != 1) {
    std::cerr << "declared context did not request one callback snapshot\n";
    return false;
  }
  constexpr auto firstExpected = 0.8022F;
  for (int frame = 0; frame < firstAudio.getNumSamples(); ++frame) {
    if (std::abs(firstAudio.getSample(0, frame) - firstExpected) >= 1.0e-5F ||
        std::abs(firstAudio.getSample(1, frame) - 0.1F) >= 1.0e-6F) {
      std::cerr << "available host context was not dispatched at BEGIN_BLOCK\n";
      return false;
    }
  }

  juce::AudioPlayHead::PositionInfo secondPosition;
  secondPosition.setTimeInSamples(100);
  secondPosition.setTimeInSeconds(2.0);
  secondPosition.setPpqPosition(8.0);
  playHead.setPosition(secondPosition);
  processor.setNonRealtime(true);

  juce::AudioBuffer<float> secondAudio(2, 4);
  juce::MidiBuffer boundaryMidi;
  boundaryMidi.addEvent(juce::MidiMessage::controllerEvent(1, 74, 64), 2);
  processor.processBlock(secondAudio, boundaryMidi);
  if (playHead.positionRequests() != 2) {
    std::cerr
        << "context capture requested more than one snapshot per callback\n";
    return false;
  }
  const auto projectedExpected =
      0.102F + static_cast<float>((2.0 + 2.0 / 48'000.0) / 100.0) + 0.12F +
      0.004F + 0.003F + 0.0034F + 0.0018F + 0.25F + 64.0F / 127.0F;
  for (int frame = 0; frame < secondAudio.getNumSamples(); ++frame) {
    const auto expected = frame < 2 ? firstExpected : projectedExpected;
    const auto expectedSample = frame < 2 ? 0.1F : 0.102F;
    if (std::abs(secondAudio.getSample(0, frame) - expected) >= 1.0e-5F ||
        std::abs(secondAudio.getSample(1, frame) - expectedSample) >= 1.0e-6F) {
      std::cerr << "host context projection, availability, render mode, or "
                   "MIDI ordering failed\n";
      return false;
    }
  }
  return true;
}

bool exerciseExplicitReset() {
  TemporarySource source;
  if (!source.write(resetEffectSource))
    return false;

  onda::plugin::Processor processor(onda::plugin::Product::effect);
  processor.prepareToPlay(48'000.0, 64);
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForOutput(processor, 0.5F))
    return false;

  processor.setSlotValue(0, 0.8F);
  if (!waitForOutput(processor, 0.8F))
    return false;

  juce::AudioBuffer<float> audio(2, 64);
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 64), 0);
  for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(), 1.0F);
  processor.processBlock(audio, midi);
  const auto changedState = 0.8F * (64.0F / 127.0F);
  if (std::abs(audio.getSample(0, 0) - changedState) >= 1.0e-5F) {
    std::cerr << "reset test did not establish modified instance state\n";
    return false;
  }

  processor.resetParametersToDefaults();
  const auto parameterResetOutput = 0.25F * (64.0F / 127.0F);
  if (!waitForOutput(processor, parameterResetOutput) ||
      std::abs(processor.slotValue(0) - 0.25F) >= 1.0e-6F) {
    std::cerr << "parameter reset did not restore the declared default\n";
    return false;
  }
  for (std::size_t index = 1; index < onda::plugin::slotCount; ++index) {
    if (std::abs(processor.slotValue(index) - 0.5F) >= 1.0e-6F) {
      std::cerr << "parameter reset did not restore an unmapped host slot\n";
      return false;
    }
  }

  processor.setSlotValue(0, 0.8F);
  if (!waitForOutput(processor, changedState))
    return false;

  processor.requestUserReset();
  if (!waitForOutput(processor, 0.25F) ||
      std::abs(processor.slotValue(0) - 0.25F) >= 1.0e-6F) {
    std::cerr
        << "explicit Reset did not restore parameters and instance state\n";
    return false;
  }
  for (std::size_t index = 1; index < onda::plugin::slotCount; ++index) {
    if (std::abs(processor.slotValue(index) - 0.5F) >= 1.0e-6F) {
      std::cerr << "explicit Reset did not restore an unmapped host slot\n";
      return false;
    }
  }
  return true;
}

bool exerciseSupersededHandoff() {
  TemporarySource source;
  if (!source.write(validSource(onda::plugin::Product::effect)))
    return false;

  onda::plugin::Processor processor(onda::plugin::Product::effect);
  processor.prepareToPlay(48'000.0, 64);
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForPublishedReplacement(processor)) {
    std::cerr << "initial replacement was not published\n";
    return false;
  }

  if (!source.write("this is not valid Onda source\n"))
    return false;
  processor.loadFile(juce::File(source.path().string()), false);

  juce::AudioBuffer<float> audio(2, 64);
  juce::MidiBuffer midi;
  for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(), 1.0F);
  processor.processBlock(audio, midi);
  for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
    for (int frame = 0; frame < audio.getNumSamples(); ++frame) {
      if (std::abs(audio.getSample(channel, frame) - 1.0F) >= 1.0e-6F) {
        std::cerr << "superseded replacement became active\n";
        return false;
      }
    }
  }
  return true;
}

bool exerciseConcurrentInstances() {
  TemporarySource effectSource;
  TemporarySource instrumentSource;
  if (!effectSource.write(validSource(onda::plugin::Product::effect)) ||
      !instrumentSource.write(validSource(onda::plugin::Product::instrument))) {
    return false;
  }

  onda::plugin::Processor effect(onda::plugin::Product::effect);
  onda::plugin::Processor instrument(onda::plugin::Product::instrument);
  effect.prepareToPlay(48'000.0, 64);
  instrument.prepareToPlay(48'000.0, 64);
  effect.loadFile(juce::File(effectSource.path().string()), false);
  instrument.loadFile(juce::File(instrumentSource.path().string()), false);
  if (!waitForOutput(effect, 0.25F) || !waitForOutput(instrument, 0.25F)) {
    std::cerr << "independent plugin instances did not become active\n";
    return false;
  }

  std::atomic<bool> succeeded{true};
  const auto process = [&succeeded](onda::plugin::Processor &processor) {
    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer midi;
    allocation_audit::enabled = true;
    for (int iteration = 0; iteration < 100; ++iteration) {
      for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(),
                    1.0F);
      processor.processBlock(audio, midi);
      for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
        for (int frame = 0; frame < audio.getNumSamples(); ++frame) {
          if (std::abs(audio.getSample(channel, frame) - 0.25F) >= 1.0e-5F)
            succeeded.store(false, std::memory_order_relaxed);
        }
      }
    }
    allocation_audit::enabled = false;
  };
  allocation_audit::count.store(0, std::memory_order_relaxed);
  std::thread effectThread(process, std::ref(effect));
  std::thread instrumentThread(process, std::ref(instrument));
  effectThread.join();
  instrumentThread.join();
  if (!succeeded.load(std::memory_order_relaxed) ||
      allocation_audit::count.load(std::memory_order_relaxed) != 0) {
    std::cerr << "concurrent JIT instances produced incorrect output\n";
    return false;
  }
  return true;
}

} // namespace

int main() {
#if defined(__linux__)
  std::signal(SIGPIPE, SIG_IGN);
#endif
  juce::ScopedJuceInitialiser_GUI juceInitialiser;
  if (!exerciseRunViewAdapter() || !exercise(onda::plugin::Product::effect) ||
      !exercise(onda::plugin::Product::instrument) || !exerciseStateRestore() ||
      !exerciseEditorLifecycle() || !exerciseAudioFileBuffers() ||
      !exerciseSourceGraphFallbackAndDiskAuthority() ||
      !exerciseProjectExportRelinksAuthority() || !exerciseExtendedMidi() ||
      !exerciseEventMetadataGating() || !exerciseHostContext() ||
      !exerciseExplicitReset() || !exerciseSupersededHandoff() ||
      !exerciseConcurrentInstances()) {
    return 1;
  }
  std::cout << "onda_processor_tests: passed\n";
  return 0;
}
