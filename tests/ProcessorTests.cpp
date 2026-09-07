#include "TemporaryDirectory.h"

#include "AudioFile.h"
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
#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace allocation_audit {
thread_local bool enabled{};
std::atomic<std::uint64_t> count{};

void record() noexcept {
  if (enabled)
    count.fetch_add(1, std::memory_order_relaxed);
}

void *allocateAligned(const std::size_t size, const std::size_t alignment) {
  const auto requested = std::max(size, std::size_t{1});
#if defined(_MSC_VER)
  return _aligned_malloc(requested, alignment);
#else
  if (requested > std::numeric_limits<std::size_t>::max() - (alignment - 1U))
    return nullptr;
  const auto padded = ((requested + alignment - 1U) / alignment) * alignment;
  return std::aligned_alloc(alignment, padded);
#endif
}

void freeAligned(void *memory) noexcept {
#if defined(_MSC_VER)
  if (memory != nullptr)
    record();
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}
} // namespace allocation_audit

namespace lock_audit {
thread_local bool enabled{};
std::atomic<std::uint64_t> count{};
} // namespace lock_audit

class StateChangeListener final : public juce::AudioProcessorListener {
public:
  explicit StateChangeListener(juce::AudioProcessor &processor)
      : processor_(processor) {
    processor_.addListener(this);
  }

  ~StateChangeListener() override { processor_.removeListener(this); }

  void audioProcessorParameterChanged(juce::AudioProcessor *, int,
                                      float) override {}

  void audioProcessorChanged(juce::AudioProcessor *,
                             const ChangeDetails &details) override {
    if (details.nonParameterStateChanged)
      ++nonParameterChanges;
  }

  int nonParameterChanges{};

private:
  juce::AudioProcessor &processor_;
};

#if defined(__linux__)
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (lock_audit::enabled)
    lock_audit::count.fetch_add(1, std::memory_order_relaxed);
  return __real_pthread_mutex_lock(mutex);
}
extern "C" void *__real_malloc(std::size_t);
extern "C" void *__real_calloc(std::size_t, std::size_t);
extern "C" void *__real_realloc(void *, std::size_t);
extern "C" void __real_free(void *);
extern "C" void *__wrap_malloc(const std::size_t size) {
  allocation_audit::record();
  return __real_malloc(size);
}
extern "C" void *__wrap_calloc(const std::size_t count,
                               const std::size_t size) {
  allocation_audit::record();
  return __real_calloc(count, size);
}
extern "C" void *__wrap_realloc(void *pointer, const std::size_t size) {
  allocation_audit::record();
  return __real_realloc(pointer, size);
}
extern "C" void __wrap_free(void *pointer) {
  if (pointer != nullptr)
    allocation_audit::record();
  __real_free(pointer);
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
  if (auto *memory = allocation_audit::allocateAligned(
          size, static_cast<std::size_t>(alignment)))
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
  allocation_audit::freeAligned(memory);
}
void operator delete[](void *memory, std::align_val_t) noexcept {
  allocation_audit::freeAligned(memory);
}
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
  allocation_audit::freeAligned(memory);
}
void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept {
  allocation_audit::freeAligned(memory);
}
void operator delete(void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
void operator delete(void *memory, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  allocation_audit::freeAligned(memory);
}
void operator delete[](void *memory, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  allocation_audit::freeAligned(memory);
}

namespace {

class TemporarySource final {
public:
  TemporarySource() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = testTemporaryRoot() /
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
    directory_ = testTemporaryRoot() /
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

constexpr auto runtimeLoggingEffectSource = R"(
ins { in1, in2 }
outs { out1, out2 }
delegate observed(value: i32)
init {
  pin preserved = i32(0)
  transient = i32(0)
  flood = false
  print("init", preserved, transient)
}
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {
  flood = true
}
sample {
  if transient == 0 {
    preserved = preserved + 1
    transient = 1
    print("sample", preserved, transient)
    observed(preserved)
  }
  if flood {
    print("flood", preserved)
  }
  out1 = in1 * 0.25
  out2 = in2 * 0.25
}
)";

constexpr auto replacementLoggingEffectSource = R"(
ins { in1, in2 }
outs { out1, out2 }
init {
  print("replacement init")
}
sample {
  out1 = in1 * 0.5
  out2 = in2 * 0.5
}
)";

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

constexpr auto userEventEffectSource = R"(
ins { in1, in2 }
outs { out1, out2 }
init { held = 0.125 }
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {}
event tempo(bpm: f64) {}
event shape(level: f32 = 0.25, offsets: i32[2] = [1, 2], enabled: bool = true, gains: f32[], token: i64 = 9007199254740993) {
  held = 0.0
  if enabled && token == 9007199254740993 {
    held = level + f32(offsets[0]) + gains[0]
  }
}
sample {
  out1 = held
  out2 = held
}
)";

constexpr std::uint32_t stateMagic = 0x41444e4fU;
constexpr int stateVersion = 3;

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
              bufferBindings = {},
          const onda::plugin::ParamControlLayout layout =
              onda::plugin::ParamControlLayout::sliders) {
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
  stream.writeBool(layout == onda::plugin::ParamControlLayout::knobs);
  return state;
}

bool processWithoutAllocation(onda::plugin::Processor &processor,
                              const float expected) {
  juce::AudioBuffer<float> audio(2, 64);
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), 16);
  midi.addEvent(juce::MidiMessage::controllerEvent(1, 74, 96), 32);
  const std::array<std::uint8_t, 256> sysex{};
  midi.addEvent(juce::MidiMessage::createSysExMessage(
                    sysex.data(), static_cast<int>(sysex.size())),
                40);
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
  const std::string_view html{
      reinterpret_cast<const char *>(root->data.data() + 3U),
      root->data.size() - 3U};
  if (html.find("id=\"clear-log\"") == std::string_view::npos ||
      html.find("state.logEntries") == std::string_view::npos ||
      html.find("entry.source") == std::string_view::npos ||
      html.find("delegateOverflowCount") == std::string_view::npos ||
      html.find("id=\"midi-keyboard\"") == std::string_view::npos ||
      html.find("midiKeyboardInteractive") == std::string_view::npos ||
      html.find("setMonitoredMidiNotes(message.activeNotes)") ==
          std::string_view::npos) {
    std::cerr << "embedded run view does not contain the 0.8.2 UI\n";
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
      !static_cast<bool>(state->getProperty("supportsScope")) ||
      static_cast<bool>(state->getProperty("midiKeyboardInteractive")) ||
      state->getProperty("paramLayout").toString() != "sliders" ||
      static_cast<bool>(state->getProperty("canExportProject")) ||
      static_cast<bool>(state->getProperty("logRevealed")) ||
      state->getProperty("logText").toString().isNotEmpty() ||
      state->getProperty("logEntries").getArray() == nullptr ||
      static_cast<juce::int64>(state->getProperty("printOverflowCount")) != 0 ||
      static_cast<juce::int64>(state->getProperty("delegateOverflowCount")) !=
          0 ||
      state->getProperty("error").toString() != "adapter test error") {
    std::cerr << "run-view capability state was invalid\n";
    return false;
  }

  onda::plugin::MidiActivitySnapshot activity;
  activity.notes[0] = std::uint64_t{1} << 60U;
  activity.notes[1] = std::uint64_t{1} << (127U - 64U);
  const auto midiMessage = onda::plugin::makeRunViewMidiActivity(activity);
  const auto *midiEnvelope = midiMessage.getDynamicObject();
  const auto *activeNotes =
      midiEnvelope == nullptr
          ? nullptr
          : midiEnvelope->getProperty("activeNotes").getArray();
  if (midiEnvelope == nullptr ||
      midiEnvelope->getProperty("type").toString() != "midiActivity" ||
      activeNotes == nullptr || activeNotes->size() != 2 ||
      static_cast<int>(activeNotes->getReference(0)) != 60 ||
      static_cast<int>(activeNotes->getReference(1)) != 127) {
    std::cerr << "run-view MIDI activity message was invalid\n";
    return false;
  }

  processor.setParamControlLayout(onda::plugin::ParamControlLayout::knobs);
  const auto knobMessage = onda::plugin::makeRunViewState(
      processor, processor.workerStatus(), processor.canExportProject(), {});
  const auto *knobEnvelope = knobMessage.getDynamicObject();
  const auto knobStateValue = knobEnvelope == nullptr
                                  ? juce::var{}
                                  : knobEnvelope->getProperty("state");
  const auto *knobState = knobStateValue.getDynamicObject();
  if (knobState == nullptr ||
      knobState->getProperty("paramLayout").toString() != "knobs") {
    std::cerr << "run-view parameter layout state was invalid\n";
    return false;
  }

  const auto scopeMessage = onda::plugin::makeRunViewScope(processor);
  const auto *scope = scopeMessage.getDynamicObject();
  if (scope == nullptr ||
      scope->getProperty("type").toString() != "scopeData" ||
      static_cast<int>(scope->getProperty("channels")) != 0 ||
      scope->getProperty("samples").getArray() == nullptr ||
      !scope->getProperty("samples").getArray()->isEmpty()) {
    std::cerr << "empty run-view scope message was invalid\n";
    return false;
  }
  return true;
}

bool exerciseUserEvents() {
  TemporarySource source;
  if (!source.write(userEventEffectSource))
    return false;

  onda::plugin::Processor processor(onda::plugin::Product::effect);
  processor.prepareToPlay(48'000.0, 64);
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForOutput(processor, 0.125F)) {
    std::cerr << "user-event processor did not become active\n";
    return false;
  }

  const auto status = processor.workerStatus();
  if (status.events.size() != 1U || status.events[0].name != "shape" ||
      status.events[0].parameters.size() != 5U ||
      status.events[0].parameters[1].type != "i32[2]" ||
      status.events[0].parameters[2].type != "bool" ||
      status.events[0].parameters[3].type != "f32[]" ||
      status.events[0].parameters[4].type != "i64") {
    std::cerr << "plugin MIDI/host events were not filtered from the view\n";
    return false;
  }

  const auto message = onda::plugin::makeRunViewState(
      processor, status, processor.canExportProject(), {});
  const auto *envelope = message.getDynamicObject();
  const auto stateValue =
      envelope == nullptr ? juce::var{} : envelope->getProperty("state");
  const auto *state = stateValue.getDynamicObject();
  const auto *events =
      state == nullptr ? nullptr : state->getProperty("events").getArray();
  const auto *event = events == nullptr || events->size() != 1
                          ? nullptr
                          : events->getReference(0).getDynamicObject();
  const auto *arguments =
      event == nullptr ? nullptr : event->getProperty("args").getArray();
  if (event == nullptr || event->getProperty("name").toString() != "shape" ||
      arguments == nullptr || arguments->size() != 5 ||
      arguments->getReference(1).getDynamicObject()->getProperty("type") !=
          "i32[2]" ||
      !static_cast<bool>(
          arguments->getReference(3).getDynamicObject()->getProperty(
              "isSlice")) ||
      arguments->getReference(4).getDynamicObject()->getProperty("default") !=
          "9007199254740993") {
    std::cerr << "user-event metadata was not adapted for the run view\n";
    return false;
  }

  for (const auto &argument : *arguments) {
    if (argument.getDynamicObject()->hasProperty("value")) {
      std::cerr << "ordinary state refresh overwrites edited event arguments\n";
      return false;
    }
  }
  const auto resetMessage = onda::plugin::makeRunViewState(
      processor, status, processor.canExportProject(), {}, true);
  const auto &resetArguments = *resetMessage["state"]["events"]
                                    .getArray()
                                    ->getReference(0)["args"]
                                    .getArray();
  for (const auto &argument : resetArguments) {
    if (!argument.getDynamicObject()->hasProperty("value") ||
        argument["value"] != argument["default"]) {
      std::cerr << "new-program state did not initialize event arguments\n";
      return false;
    }
  }

  juce::Array<juce::var> fixed{2, 3};
  juce::Array<juce::var> slice{0.25};
  juce::Array<juce::var> values{0.5, juce::var{fixed}, true, juce::var{slice},
                                "9007199254740993"};
  const auto error = processor.triggerEvent("shape", juce::var{values});
  if (!error.empty() || !waitForOutput(processor, 2.75F)) {
    std::cerr << "user event was not dispatched: " << error << '\n';
    return false;
  }

  values.set(4, 9'007'199'254'740'992.0);
  if (processor.triggerEvent("shape", juce::var{values}).empty()) {
    std::cerr << "lossy numeric i64 event argument was accepted\n";
    return false;
  }

  processor.unload();
  const auto unloaded = processor.workerStatus();
  if (unloaded.active || unloaded.engineGeneration != 0 ||
      !unloaded.mappings.empty() || !unloaded.buffers.empty() ||
      !unloaded.events.empty()) {
    std::cerr << "unload retained a stale published interface\n";
    return false;
  }
  return true;
}

bool exerciseScopeCapture() {
  TemporarySource source;
  if (!source.write(validSource(onda::plugin::Product::effect)))
    return false;

  onda::plugin::Processor processor(onda::plugin::Product::effect);
  processor.prepareToPlay(48'000.0, 64);
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForOutput(processor, 0.25F)) {
    std::cerr << "scope processor did not become active\n";
    return false;
  }

  processor.setScopeCaptureEnabled(true);
  juce::AudioBuffer<float> audio(2, 64);
  juce::MidiBuffer midi;
  allocation_audit::count.store(0, std::memory_order_relaxed);
  lock_audit::count.store(0, std::memory_order_relaxed);
  allocation_audit::enabled = true;
  lock_audit::enabled = true;
  constexpr auto renderedFrames = 80 * 64;
  for (auto firstFrame = 0; firstFrame < renderedFrames; firstFrame += 64) {
    for (auto frame = 0; frame < audio.getNumSamples(); ++frame) {
      const auto value = static_cast<float>(firstFrame + frame) / 4096.0F;
      audio.setSample(0, frame, value);
      audio.setSample(1, frame, -value);
    }
    processor.processBlock(audio, midi);
  }
  lock_audit::enabled = false;
  allocation_audit::enabled = false;
  if (allocation_audit::count.load(std::memory_order_relaxed) != 0 ||
      lock_audit::count.load(std::memory_order_relaxed) != 0) {
    std::cerr << "scope capture was not realtime-safe\n";
    return false;
  }

  const auto snapshot = processor.scopeSnapshot();
  const auto firstCapturedFrame =
      renderedFrames -
      static_cast<int>(onda::plugin::ScopeCapture::snapshotFrames);
  if (snapshot.channels != 2 ||
      snapshot.samples.size() !=
          onda::plugin::ScopeCapture::snapshotFrames * 2U) {
    std::cerr << "scope snapshot dimensions were invalid\n";
    return false;
  }
  for (std::size_t frame = 0;
       frame < onda::plugin::ScopeCapture::snapshotFrames; ++frame) {
    const auto expected =
        static_cast<float>(firstCapturedFrame + static_cast<int>(frame)) /
        16384.0F;
    if (std::abs(snapshot.samples[frame * 2U] - expected) >= 1.0e-6F ||
        std::abs(snapshot.samples[frame * 2U + 1U] + expected) >= 1.0e-6F) {
      std::cerr << "scope snapshot was not the latest interleaved output\n";
      return false;
    }
  }

  const auto message = onda::plugin::makeRunViewScope(processor);
  const auto *scope = message.getDynamicObject();
  const auto *samples =
      scope == nullptr ? nullptr : scope->getProperty("samples").getArray();
  if (scope == nullptr ||
      scope->getProperty("type").toString() != "scopeData" ||
      static_cast<int>(scope->getProperty("channels")) != 2 ||
      samples == nullptr ||
      samples->size() != static_cast<int>(snapshot.samples.size())) {
    std::cerr << "captured run-view scope message was invalid\n";
    return false;
  }

  processor.setScopeCaptureEnabled(false);
  if (processor.scopeSnapshot().channels != 0 ||
      !processor.scopeSnapshot().samples.empty()) {
    std::cerr << "disabled scope capture remained visible\n";
    return false;
  }
  return true;
}

bool exerciseRuntimeLogging() {
  TemporarySource source;
  if (!source.write(runtimeLoggingEffectSource))
    return false;

  onda::plugin::Processor processor(onda::plugin::Product::effect);
  processor.prepareToPlay(48'000.0, 64);
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForOutput(processor, 0.25F)) {
    std::cerr << "logging processor did not become active\n";
    return false;
  }

  const auto waitForRecords = [&processor](const std::size_t count) {
    for (int attempt = 0; attempt < 100; ++attempt) {
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
      auto snapshot = processor.runtimeLogSnapshot();
      if (snapshot.records.size() >= count)
        return snapshot;
    }
    return processor.runtimeLogSnapshot();
  };
  auto snapshot = waitForRecords(3U);
  if (!snapshot.revealed || snapshot.records.size() != 3U ||
      snapshot.records[0].text != "init: 0 0" ||
      snapshot.records[1].text != "sample: 1 1" ||
      snapshot.records[2].text != "delegate observed: value=1" ||
      snapshot.records[0].sourceFile.empty() ||
      snapshot.records[0].lexicalOwner.empty()) {
    std::cerr << "processor runtime output was not drained in order\n";
    return false;
  }

  const auto message = onda::plugin::makeRunViewState(
      processor, processor.workerStatus(), processor.canExportProject(), {});
  const auto *envelope = message.getDynamicObject();
  const auto stateValue =
      envelope == nullptr ? juce::var{} : envelope->getProperty("state");
  const auto *state = stateValue.getDynamicObject();
  const auto *entries =
      state == nullptr ? nullptr : state->getProperty("logEntries").getArray();
  const auto *firstEntry = entries == nullptr || entries->isEmpty()
                               ? nullptr
                               : entries->getReference(0).getDynamicObject();
  const auto sourceValue =
      firstEntry == nullptr ? juce::var{} : firstEntry->getProperty("source");
  const auto *sourceContext = sourceValue.getDynamicObject();
  if (state == nullptr ||
      !static_cast<bool>(state->getProperty("logRevealed")) ||
      !state->getProperty("logText").toString().contains("sample: 1 1") ||
      entries == nullptr || entries->size() != 3 || sourceContext == nullptr ||
      sourceContext->getProperty("file").toString().isEmpty() ||
      static_cast<juce::int64>(sourceContext->getProperty("line")) == 0) {
    std::cerr << "runtime output was not adapted to the logger view\n";
    return false;
  }

  processor.clearRuntimeLog();
  snapshot = processor.runtimeLogSnapshot();
  if (!snapshot.revealed || !snapshot.records.empty() ||
      snapshot.counters.printOverflow != 0U ||
      snapshot.counters.delegateOverflow != 0U) {
    std::cerr << "clearing the runtime log did not preserve its reveal state\n";
    return false;
  }

  processor.reset();
  if (!processWithoutAllocation(processor, 0.25F)) {
    std::cerr << "reset logging was not realtime safe\n";
    return false;
  }
  for (int index = 0; index < 4; ++index) {
    if (!processWithoutAllocation(processor, 0.25F))
      return false;
  }
  snapshot = waitForRecords(3U);
  if (snapshot.records.size() < 3U || snapshot.records[0].text != "init: 1 0" ||
      snapshot.records[1].text != "sample: 2 1" ||
      snapshot.records[2].text != "delegate observed: value=2" ||
      snapshot.counters.printTransportDrops == 0U) {
    std::cerr << "host reset did not preserve pinned logging state\n";
    return false;
  }

  processor.reset();
  if (!processWithoutAllocation(processor, 0.25F))
    return false;
  processor.clearRuntimeLog();
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  snapshot = processor.runtimeLogSnapshot();
  if (!snapshot.revealed || !snapshot.records.empty() ||
      snapshot.counters.printTransportDrops != 0U ||
      snapshot.counters.delegateTransportDrops != 0U) {
    std::cerr << "clear-log retained queued output from the previous epoch\n";
    return false;
  }

  for (int block = 0; block < 18; ++block) {
    if (!processWithoutAllocation(processor, 0.25F))
      return false;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
  }
  snapshot = processor.runtimeLogSnapshot();
  if (snapshot.records.size() > 1024U ||
      snapshot.counters.printTransportDrops == 0U) {
    std::cerr << "runtime log retention was not bounded and accounted for\n";
    return false;
  }
  processor.clearRuntimeLog();

  for (int index = 0; index < 4; ++index) {
    if (!processWithoutAllocation(processor, 0.25F))
      return false;
  }
  const auto previousRevision = processor.workerStatus().revision;
  if (!source.write(replacementLoggingEffectSource))
    return false;
  processor.requestReload();
  auto replacementReady = false;
  for (int attempt = 0; attempt < 500; ++attempt) {
    const auto status = processor.workerStatus();
    if (status.revision > previousRevision && status.active &&
        !status.compiling && status.message == "Active") {
      replacementReady = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!replacementReady || !processWithoutAllocation(processor, 0.5F)) {
    std::cerr << "replacement logging processor did not become active\n";
    return false;
  }
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  if (!processWithoutAllocation(processor, 0.5F))
    return false;
  snapshot = waitForRecords(1U);
  if (!snapshot.revealed || snapshot.records.size() != 1U ||
      snapshot.records[0].text != "replacement init") {
    std::cerr << "replacement init log was crowded out by stale output\n";
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
    original.setParamControlLayout(onda::plugin::ParamControlLayout::knobs);
    original.getStateInformation(state);

    const auto unchanged = [&] {
      const auto [width, height] = original.editorSize();
      return std::abs(original.slotValue(0) - 0.8125F) < 1.0e-6F &&
             width == 777 && height == 888 &&
             original.paramControlLayout() ==
                 onda::plugin::ParamControlLayout::knobs &&
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
        height != 888 ||
        restored.paramControlLayout() !=
            onda::plugin::ParamControlLayout::knobs ||
        restored.workerStatus().path != source.path()) {
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

  StateChangeListener stateChanges{processor};

  std::unique_ptr<juce::AudioProcessorEditor> editor{
      processor.createEditorAndMakeActive()};
  if (!editor || processor.getActiveEditor() != editor.get()) {
    std::cerr << "processor did not create and retain its editor\n";
    return false;
  }
  const auto *loadingOverlay = dynamic_cast<juce::Label *>(
      editor->findChildWithID("onda-loading-overlay"));
  if (loadingOverlay == nullptr ||
      loadingOverlay->getBounds() != editor->getLocalBounds() ||
      loadingOverlay->findColour(juce::Label::backgroundColourId) !=
          juce::Colour::fromRGB(8, 21, 34)) {
    std::cerr << "editor did not cover the initializing web view\n";
    return false;
  }
#if JUCE_WINDOWS
  editor->addToDesktop(0);
#endif
  editor->setVisible(true);
#if JUCE_WINDOWS
  for (int attempt = 0; attempt < 1000 && loadingOverlay->isVisible();
       ++attempt)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  if (loadingOverlay->isVisible()) {
    std::cerr << "Windows editor did not complete its WebView2 handshake: "
              << loadingOverlay->getText() << '\n';
    processor.editorBeingDeleted(editor.get());
    return false;
  }
#endif
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  editor->setSize(777, 888);
  processor.setParamControlLayout(onda::plugin::ParamControlLayout::knobs);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  const auto [resizedWidth, resizedHeight] = processor.editorSize();
  if (resizedWidth != 777 || resizedHeight != 888 ||
      processor.paramControlLayout() !=
          onda::plugin::ParamControlLayout::knobs ||
      stateChanges.nonParameterChanges < 2 ||
      !automate(0.4F, "with the editor open")) {
    std::cerr << "editor preferences did not mark plugin state dirty\n";
    return false;
  }

  editor->setVisible(false);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  if (!automate(0.6F, "with the editor unavailable"))
    return false;

  processor.editorBeingDeleted(editor.get());
  editor.reset();
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  if (processor.getActiveEditor() != nullptr)
    return false;

  const auto [storedWidth, storedHeight] = processor.editorSize();
  editor.reset(processor.createEditorAndMakeActive());
  if (!editor || editor->getWidth() != 777 || editor->getHeight() != 888) {
    std::cerr << "recreated editor did not retain its dimensions (stored "
              << storedWidth << 'x' << storedHeight << ", reopened "
              << (editor ? editor->getWidth() : 0) << 'x'
              << (editor ? editor->getHeight() : 0) << ")\n";
    return false;
  }
  processor.editorBeingDeleted(editor.get());
  editor.reset();
  if (processor.getActiveEditor() != nullptr ||
      !automate(0.8F, "after editor destruction")) {
    return false;
  }

  processor.releaseResources();
  return true;
}

bool exerciseClearedBufferStateRestore() {
  TemporarySource source;
  TemporaryAudioFile clip{"cleared-clip"};
  TemporaryAudioFile retained{"retained-clip"};
  if (!source.write(R"(
buffers { clip: buffer<f32[2]>, retained: buffer<f32[2]> }
outs { out1, out2 }
sample { out1 = clip[0, 0] + retained[1, 0]; out2 = clip[0, 0] + retained[1, 0] }
)") || !clip.write(0.125F, 0.25F) ||
      !retained.write(0.125F, 0.25F))
    return false;

  using namespace onda::plugin;
  Processor processor(Product::effect);
  processor.loadFile(juce::File(source.path().string()), false);
  processor.bindBufferFile("clip", juce::File(clip.path().string()));
  processor.bindBufferFile("retained", juce::File(retained.path().string()));
  processor.prepareToPlay(48'000.0, 8);
  if (!waitForOutput(processor, 0.375F, 8))
    return false;
  processor.clearBuffer("clip");
  for (const bool waitForBuild : {false, true}) {
    if (waitForBuild)
      processor.prepareToPlay(48'000.0, 8);
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    Processor restored(Product::effect);
    restored.setStateInformation(saved.getData(),
                                 static_cast<int>(saved.getSize()));
    restored.prepareToPlay(48'000.0, 8);
    const auto status = restored.workerStatus();
    const auto kept =
        std::ranges::find(status.buffers, "retained", &BufferMapping::name);
    const auto cleared =
        std::ranges::find(status.buffers, "clip", &BufferMapping::name);
    if (status.path != source.path() || status.active ||
        restored.canExportProject() || kept == status.buffers.end() ||
        kept->loadedPath != retained.path() ||
        cleared == status.buffers.end() || !cleared->loadedPath.empty()) {
      std::cerr
          << "cleared-buffer state lost its source or remaining binding\n";
      return false;
    }
    restored.bindBufferFile("clip", juce::File(clip.path().string()));
    restored.prepareToPlay(48'000.0, 8);
    if (!waitForOutput(restored, 0.375F, 8))
      return false;
  }
  return true;
}

bool exerciseProjectBufferOverrideRestore() {
  TemporarySource source;
  if (!source.write(R"(
config const Gain: f32 = 1.0
buffers { clip: buffer<f32[2]>, untouched: buffer<f32> }
outs { out1, out2 }
sample {
  out1 = clip[0, 0] * Gain + untouched[0]
  out2 = clip[0, 0] * Gain + untouched[0]
}
)"))
    return false;
  const auto projectPath = source.path().parent_path() / "override.ondaproject";
  {
    std::ofstream manifest(projectPath);
    manifest << "{\"entry\":\"" << source.path().filename().string()
             << R"(","constants":{"Gain":2.0},"buffers":{
    "clip":{"inline":{"element":"f32","channels":2,"sample_rate":48000,"values":[0.25,0.5]}},
    "untouched":{"inline":{"element":"f32","channels":1,"sample_rate":48000,"values":[0.125]}}
  }})";
    if (!manifest.good())
      return false;
  }
  TemporaryAudioFile audio{"onda-project-override"};
  if (!audio.write(0.75F, 0.25F))
    return false;
  const auto decoded = onda::plugin::decodeAudioFile(audio.path());
  if (!decoded.audio)
    return false;
  // Compare the checkpoint exactly, accounting for the FLAC fixture's 16-bit
  // quantization before applying the project constant and unchanged buffer.
  const auto expectedOverride =
      decoded.audio->interleavedSamples[0] * 2.0F + 0.125F;
  juce::MemoryBlock saved;
  {
    onda::plugin::Processor processor(onda::plugin::Product::effect);
    StateChangeListener listener(processor);
    processor.prepareToPlay(48'000.0, 8);
    processor.loadFile(juce::File(projectPath.string()), false);
    if (!waitForOutput(processor, 0.625F, 8)) {
      std::cerr << "project default failed: " << processor.workerStatus().message << '\n';
      return false;
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    auto changes = listener.nonParameterChanges;
    processor.bindBufferFile("clip", juce::File(audio.path().string()));
    if (!waitForOutput(processor, expectedOverride, 8)) {
      std::cerr << "project override failed: " << processor.workerStatus().message << '\n';
      return false;
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    if (listener.nonParameterChanges <= changes) {
      std::cerr << "buffer override did not mark host state dirty\n";
      return false;
    }
    changes = listener.nonParameterChanges;
    processor.getStateInformation(saved);
    processor.clearBuffer("clip");
    if (!waitForOutput(processor, 0.625F, 8)) {
      std::cerr << "clearing an override did not restore the project default\n";
      return false;
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    if (listener.nonParameterChanges <= changes) {
      std::cerr << "clearing a buffer override did not mark host state dirty\n";
      return false;
    }
  }
  // Missing external audio alone must trigger the exact saved override.
  std::filesystem::remove(audio.path());
  {
    onda::plugin::Processor restored(onda::plugin::Product::effect);
    restored.setStateInformation(saved.getData(),
                                 static_cast<int>(saved.getSize()));
    restored.prepareToPlay(48'000.0, 8);
    if (!waitForProjectImageOutput(restored, expectedOverride, 8)) {
      std::cerr << "saved project lost its buffer override or constants\n";
      return false;
    }
  }
  source.remove();
  std::filesystem::remove(projectPath);
  onda::plugin::Processor portable(onda::plugin::Product::effect);
  portable.setStateInformation(saved.getData(),
                               static_cast<int>(saved.getSize()));
  portable.prepareToPlay(48'000.0, 8);
  if (!waitForProjectImageOutput(portable, expectedOverride, 8)) {
    std::cerr << "buffer override checkpoint depended on the source project\n";
    return false;
  }
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
        testTemporaryRoot() /
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
  const auto exportRoot =
      testTemporaryRoot() / ("onda-processor-export-" + std::to_string(stamp));
  std::filesystem::create_directories(exportRoot);
  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(exportRoot, ignored);
  };

  onda::plugin::Processor processor(onda::plugin::Product::effect);
  StateChangeListener listener(processor);
  processor.prepareToPlay(48'000.0, 64);
  processor.loadFile(juce::File(project.entry().string()), false);
  if (!waitForOutput(processor, 0.25F)) {
    cleanup();
    return false;
  }
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  const auto changes = listener.nonParameterChanges;
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

  processor.prepareToPlay(48'000.0, 64);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  if (listener.nonParameterChanges <= changes) {
    std::cerr << "project export relinking did not mark host state dirty\n";
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

bool exerciseMidiKeyboardMonitor() {
  TemporarySource source;
  if (!source.write(validSource(onda::plugin::Product::instrument)))
    return false;

  onda::plugin::Processor processor(onda::plugin::Product::instrument);
  processor.prepareToPlay(48'000.0, 8);
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForOutput(processor, 0.25F, 8)) {
    std::cerr << "MIDI keyboard monitor instrument did not become active\n";
    return false;
  }

  const auto status = processor.workerStatus();
  const auto stateMessage = onda::plugin::makeRunViewState(
      processor, status, processor.canExportProject(), {});
  const auto *envelope = stateMessage.getDynamicObject();
  const auto stateValue =
      envelope == nullptr ? juce::var{} : envelope->getProperty("state");
  const auto *state = stateValue.getDynamicObject();
  const auto midiValue =
      state == nullptr ? juce::var{} : state->getProperty("midi");
  const auto *midiState = midiValue.getDynamicObject();
  if (!status.midi.noteOn || !status.midi.noteOff || midiState == nullptr ||
      static_cast<bool>(midiState->getProperty("available")) ||
      !static_cast<bool>(midiState->getProperty("noteOn")) ||
      !static_cast<bool>(midiState->getProperty("noteOff"))) {
    std::cerr << "plugin MIDI keyboard capabilities were invalid\n";
    return false;
  }

  juce::AudioBuffer<float> audio(2, 8);
  const auto process = [&](juce::MidiBuffer &midi) {
    audio.clear();
    processor.processBlock(audio, midi);
  };
  const auto initialRevision = processor.midiActivitySnapshot().revision;
  juce::MidiBuffer noteOns;
  noteOns.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0F), 0);
  noteOns.addEvent(juce::MidiMessage::noteOn(2, 60, 0.5F), 1);
  noteOns.addEvent(juce::MidiMessage::noteOn(2, 64, 0.5F), 2);
  process(noteOns);
  const auto held = processor.midiActivitySnapshot();
  if (held.revision == initialRevision || !held.active(60) ||
      !held.active(64)) {
    std::cerr << "host note-on activity was not captured\n";
    return false;
  }

  juce::MidiBuffer firstRelease;
  firstRelease.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
  process(firstRelease);
  if (!processor.midiActivitySnapshot().active(60)) {
    std::cerr << "one channel released another channel's held note\n";
    return false;
  }

  juce::MidiBuffer allNotesOff;
  allNotesOff.addEvent(juce::MidiMessage::controllerEvent(2, 123, 0), 0);
  process(allNotesOff);
  const auto released = processor.midiActivitySnapshot();
  if (released.active(60) || released.active(64)) {
    std::cerr << "host all-notes-off activity was not captured\n";
    return false;
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
  secondPosition.setIsPlaying(true);
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
      0.004F + 0.003F + 0.0034F + 0.0018F + 0.01F + 0.25F + 64.0F / 127.0F;
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

bool exerciseTimelineProjection() {
  struct PositionEvent {
    const char *declaration;
    double value;
    double perFrame;
  };
  const std::array events{
      PositionEvent{"event sample_position(sample: i64) { held = f64(sample) }",
                    100.0, 1.0},
      PositionEvent{"event time_position(seconds: f64) { held = seconds }",
                    2.0, 1.0 / 48'000.0},
      PositionEvent{"event musical_position(quarter_note: f64) { held = quarter_note }",
                    4.0, 2.0 / 48'000.0},
  };
  for (const auto &event : events) {
    for (const auto playing : {false, true}) {
      for (const auto declaresTransport : {false, true}) {
        TemporarySource source;
        auto text = std::string{"outs { out1, out2 }\ninit { held = f64(0) }\n"} +
                    event.declaration +
                    "\nsample { out1 = f32(held); out2 = f32(held) }\n";
        if (declaresTransport)
          text += "event transport(playing: bool, recording: bool, looping: bool) {}\n";
        if (!source.write(text))
          return false;

        TestPlayHead playHead;
        juce::AudioPlayHead::PositionInfo position;
        position.setIsPlaying(playing);
        position.setTimeInSamples(100);
        position.setTimeInSeconds(2.0);
        position.setPpqPosition(4.0);
        // A stopped timeline needs no tempo to retain its exact PPQ.
        if (playing)
          position.setBpm(120.0);
        playHead.setPosition(position);
        onda::plugin::Processor processor(onda::plugin::Product::effect);
        processor.setPlayHead(&playHead);
        processor.loadFile(juce::File(source.path().string()), false);
        processor.prepareToPlay(48'000.0, 8);
        if (!processor.workerStatus().active)
          return false;
        juce::AudioBuffer<float> audio(2, 6);
        juce::MidiBuffer midi;
        for (int callback = 0; callback < 2; ++callback) {
          processor.processBlock(audio, midi);
          for (int frame = 0; frame < 6; ++frame) {
            const auto offset = callback == 1 && frame >= 2 && playing ? 2.0 : 0.0;
            const auto expected = static_cast<float>(event.value + offset * event.perFrame);
            if (std::abs(audio.getSample(0, frame) - expected) > 1.0e-6F) {
              std::cerr << "timeline projection ignored playback state: "
                        << event.declaration << '\n';
              return false;
            }
          }
        }
      }
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
  if (!waitForSafeEffectBypass(processor) ||
      processor.workerStatus().engineGeneration != 0) {
    std::cerr << "discarded replacement remained active in worker status\n";
    return false;
  }
  // Repeat with an adopted engine: the failure must retain its audio and
  // interface, not those of the superseded replacement.
  if (!source.write(validSource(onda::plugin::Product::effect)))
    return false;
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForOutput(processor, 0.25F))
    return false;
  const auto running = processor.workerStatus();
  if (!source.write(
          "outs { out1, out2 }\nsample { out1 = 0.75; out2 = 0.75 }\n"))
    return false;
  processor.loadFile(juce::File(source.path().string()), false);
  if (!waitForPublishedReplacement(processor))
    return false;
  if (!source.write("this is not valid Onda source\n"))
    return false;
  processor.loadFile(juce::File(source.path().string()), false);
  for (int attempt = 0; attempt < 500; ++attempt) {
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
      std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(), 1.0F);
    processor.processBlock(audio, midi);
    const auto status = processor.workerStatus();
    if (!status.compiling && status.message != "Waiting to compile" &&
        status.message != "Active") {
      if (!status.active ||
          status.engineGeneration != running.engineGeneration ||
          status.mappings.size() != running.mappings.size() ||
          std::abs(audio.getSample(0, 0) - 0.25F) >= 1.0e-5F) {
        std::cerr << "superseded replacement displaced the running interface\n";
        return false;
      }
      juce::MemoryBlock saved;
      processor.getStateInformation(saved);
      onda::plugin::Processor restored(onda::plugin::Product::effect);
      restored.setStateInformation(saved.getData(),
                                   static_cast<int>(saved.getSize()));
      restored.prepareToPlay(48'000.0, 64);
      if (!waitForProjectImageOutput(restored, 0.25F)) {
        std::cerr << "superseded replacement displaced the running checkpoint\n";
        return false;
      }
      return true;
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  }
  std::cerr << "superseded replacement failure did not finish\n";
  return false;
}

// No dispatch-loop pumping: host preparation and offline processing must be
// complete before the very first rendered sample.
bool rendersImmediately(onda::plugin::Processor &processor, const int frames,
                        const float expected) {
  juce::AudioBuffer<float> audio(2, frames);
  juce::MidiBuffer midi;
  for (int block = 0; block < 100; ++block) {
    for (int channel = 0; channel < 2; ++channel)
      std::fill_n(audio.getWritePointer(channel), frames, 1.0F);
    processor.processBlock(audio, midi);
    for (int channel = 0; channel < 2; ++channel) {
      for (int frame = 0; frame < frames; ++frame) {
        if (std::abs(audio.getSample(channel, frame) - expected) > 1.0e-6F) {
          std::cerr
              << "host preparation returned before correct audio was ready\n";
          return false;
        }
      }
    }
  }
  return true;
}

bool exerciseHostPreparation() {
  using namespace onda::plugin;
  for (const auto product : {Product::effect, Product::instrument}) {
    TemporarySource source;
    if (!source.write(validSource(product)))
      return false;
    Processor original(product);
    if (std::fpclassify(original.getTailLengthSeconds()) != FP_ZERO)
      return false;
    original.loadFile(juce::File(source.path().string()), true);
    original.prepareToPlay(48'000.0, 64);
    if (!rendersImmediately(original, 64, 0.25F) ||
        !std::isinf(original.getTailLengthSeconds()))
      return false;
    original.releaseResources();
    original.prepareToPlay(48'000.0, 64);
    if (!rendersImmediately(original, 64, 0.25F))
      return false;

    juce::MemoryBlock state;
    original.getStateInformation(state);
    Processor restored(product);
    restored.setNonRealtime(true);
    restored.setStateInformation(state.getData(),
                                 static_cast<int>(state.getSize()));
    restored.prepareToPlay(44'100.0, 512);
    if (!rendersImmediately(restored, 512, 0.25F))
      return false;
    restored.releaseResources();
    restored.prepareToPlay(96'000.0, 128);
    if (!rendersImmediately(restored, 128, 0.25F))
      return false;
    // Some hosts restore state after preparing, or change mode at process time.
    restored.setNonRealtime(false);
    restored.setStateInformation(state.getData(),
                                 static_cast<int>(state.getSize()));
    restored.setNonRealtime(true);
    if (!rendersImmediately(restored, 128, 0.25F) ||
        !rendersImmediately(restored, 256, 0.25F))
      return false;
    // A realtime callback can raise the required bound before the timer has
    // configured it. Offline rendering must finish that deferred work itself.
    restored.setNonRealtime(false);
    if (!rendersImmediately(restored, 512,
                            product == Product::effect ? 1.0F : 0.0F))
      return false;
    restored.setNonRealtime(true);
    if (!rendersImmediately(restored, 128, 0.25F) ||
        !rendersImmediately(restored, 512, 0.25F))
      return false;
    source.remove();
    restored.setStateInformation(state.getData(),
                                 static_cast<int>(state.getSize()));
    restored.prepareToPlay(48'000.0, 64);
    if (!rendersImmediately(restored, 64, 0.25F) ||
        !restored.workerStatus().usingProjectImage)
      return false;
    restored.unload();
    if (std::fpclassify(restored.getTailLengthSeconds()) != FP_ZERO)
      return false;
  }
  // Reprepare while an asynchronously loaded engine is queued for default
  // seeding. No timer may be needed to clear deactivation or set its slots.
  TemporarySource seededSource;
  if (!seededSource.write(parameterEffectSource))
    return false;
  Processor seeded(Product::effect);
  seeded.prepareToPlay(48'000.0, 64);
  seeded.setSlotValue(0, 0.9F);
  seeded.loadFile(juce::File(seededSource.path().string()), true);
  if (!waitForPublishedReplacement(seeded))
    return false;
  seeded.prepareToPlay(48'000.0, 64);
  if (!rendersImmediately(seeded, 64, 0.5F))
    return false;

  // A completed compilation failure must release the readiness waiter too.
  TemporarySource invalid;
  if (!invalid.write("invalid source\n"))
    return false;
  Processor failed(Product::effect);
  failed.loadFile(juce::File(invalid.path().string()), true);
  failed.prepareToPlay(48'000.0, 64);
  return !failed.workerStatus().active && rendersImmediately(failed, 64, 1.0F);
}

bool exerciseAutomaticEventReplacement() {
  using namespace onda::plugin;
  TemporarySource source;
  if (!source.write("outs { out1, out2 }\ninit { held = 0.125 }\n"
                    "event alpha(value: f32) { held = value }\n"
                    "sample { out1 = held; out2 = held }\n"))
    return false;
  Processor processor(Product::effect);
  processor.loadFile(juce::File(source.path().string()), false);
  processor.prepareToPlay(48'000.0, 64);
  if (!rendersImmediately(processor, 64, 0.125F))
    return false;
  const auto before = processor.workerStatus();
  if (!processor.triggerEvent("alpha", juce::Array<juce::var>{0.5}).empty() ||
      !source.write("outs { out1, out2 }\ninit { held = 0.25 }\n"
                    "event beta(values: f64[]) { held = 0.875 }\n"
                    "sample { out1 = held; out2 = held }\n") ||
      !waitForActiveRevision(processor, before.revision))
    return false;
  const auto after = processor.workerStatus();
  if (before.engineGeneration == after.engineGeneration ||
      !rendersImmediately(processor, 64, 0.25F)) {
    std::cerr << "automatic reload reused event identity or dispatched an old "
                 "payload\n";
    return false;
  }
  return processor
             .triggerEvent("beta", juce::Array<juce::var>{juce::var{
                                       juce::Array<juce::var>{1.0, 2.0}}})
             .empty() &&
         rendersImmediately(processor, 64, 0.875F);
}

bool exerciseProjectDirtyNotifications() {
  using namespace onda::plugin;
  TemporarySource source;
  if (!source.write(validSource(Product::effect)))
    return false;
  Processor processor(Product::effect);
  StateChangeListener listener(processor);
  processor.loadFile(juce::File(source.path().string()), true);
  processor.prepareToPlay(48'000.0, 64);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  if (listener.nonParameterChanges == 0) {
    std::cerr
        << "loading a parameterless project did not mark host state dirty\n";
    return false;
  }
  auto changes = listener.nonParameterChanges;
  const auto previous = processor.workerStatus().revision;
  if (!source.write(
          "outs { out1, out2 }\nsample { out1 = 0.75; out2 = 0.75 }\n") ||
      !waitForActiveRevision(processor, previous) ||
      !waitForOutput(processor, 0.75F))
    return false;
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  if (listener.nonParameterChanges <= changes)
    return false;
  changes = listener.nonParameterChanges;
  processor.requestReload();
  processor.prepareToPlay(48'000.0, 64);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  if (listener.nonParameterChanges != changes) {
    std::cerr << "identical recompilation dirtied host state\n";
    return false;
  }
  juce::MemoryBlock state;
  processor.getStateInformation(state);
  Processor restored(Product::effect);
  StateChangeListener restoreListener(restored);
  restored.setStateInformation(state.getData(),
                               static_cast<int>(state.getSize()));
  restored.prepareToPlay(44'100.0, 128);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  if (restoreListener.nonParameterChanges != 0) {
    std::cerr << "restoring host state marked it dirty\n";
    return false;
  }
  processor.unload();
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  return listener.nonParameterChanges > changes;
}

bool exerciseConcurrentStateRestoreAndLogDrain() {
  onda::plugin::Processor processor(onda::plugin::Product::effect);
  std::array<float, onda::plugin::slotCount> values{};
  const auto state = makeState({}, {}, values, 480, 720);
  std::atomic<bool> done{};
  std::thread host([&] {
    for (int iteration = 0; iteration < 300; ++iteration) {
      processor.setStateInformation(state.getData(),
                                     static_cast<int>(state.getSize()));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    done.store(true, std::memory_order_release);
  });
  while (!done.load(std::memory_order_acquire))
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  host.join();
  juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
  const auto log = processor.runtimeLogSnapshot();
  return log.records.empty() && log.counters.printOverflow == 0 &&
         log.counters.printTransportDrops == 0 &&
         log.counters.delegateOverflow == 0 &&
         log.counters.delegateTransportDrops == 0;
}

bool exerciseAlignedAllocation() {
  constexpr auto alignment = std::align_val_t{64};
  auto *scalar = ::operator new(17, alignment);
  auto *array = ::operator new[](129, alignment, std::nothrow);
  const auto aligned = [](const void *pointer) {
    return pointer != nullptr && reinterpret_cast<std::uintptr_t>(pointer) % 64U == 0;
  };
  const auto valid = aligned(scalar) && aligned(array);
  ::operator delete(scalar, alignment);
  ::operator delete[](array, alignment, std::nothrow);
  return valid;
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
  if (!exerciseAlignedAllocation() || !exerciseHostPreparation() ||
      !exerciseConcurrentStateRestoreAndLogDrain() ||
      !exerciseAutomaticEventReplacement() ||
      !exerciseProjectDirtyNotifications() || !exerciseRunViewAdapter() ||
      !exerciseScopeCapture() || !exerciseUserEvents() ||
      !exerciseRuntimeLogging() || !exercise(onda::plugin::Product::effect) ||
      !exercise(onda::plugin::Product::instrument) || !exerciseStateRestore() ||
      !exerciseEditorLifecycle() || !exerciseAudioFileBuffers() ||
      !exerciseProjectBufferOverrideRestore() ||
      !exerciseClearedBufferStateRestore() ||
      !exerciseSourceGraphFallbackAndDiskAuthority() ||
      !exerciseProjectExportRelinksAuthority() || !exerciseExtendedMidi() ||
      !exerciseMidiKeyboardMonitor() || !exerciseEventMetadataGating() ||
      !exerciseHostContext() || !exerciseTimelineProjection() ||
      !exerciseExplicitReset() ||
      !exerciseSupersededHandoff() || !exerciseConcurrentInstances()) {
    return 1;
  }
  std::cout << "onda_processor_tests: passed\n";
  return 0;
}
