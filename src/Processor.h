#pragma once

#include "Engine.h"
#include "Product.h"
#include "ScopeCapture.h"
#include "SpscQueue.h"
#include "SpscSlot.h"
#include "Worker.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace onda::plugin {

enum class ParamControlLayout : std::uint8_t { sliders, knobs };

inline constexpr std::size_t midiNoteCount = 128U;
inline constexpr std::size_t midiNoteWordCount = midiNoteCount / 64U;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

struct MidiActivitySnapshot {
  std::uint64_t revision{};
  std::array<std::uint64_t, midiNoteWordCount> notes{};

  [[nodiscard]] bool active(const std::size_t note) const noexcept {
    return note < midiNoteCount &&
           (notes[note / 64U] & (std::uint64_t{1} << (note % 64U))) != 0U;
  }
};

inline constexpr std::size_t userEventPayloadCapacity = 16U * 1024U;

struct UserEventCommand {
  std::uint64_t generation{};
  int eventIndex{-1};
  std::uint32_t payloadBytes{};
  std::array<std::byte, userEventPayloadCapacity> payload{};

  UserEventCommand &operator=(const UserEventCommand &other) noexcept {
    if (this == &other)
      return *this;
    generation = other.generation;
    eventIndex = other.eventIndex;
    payloadBytes = other.payloadBytes;
    std::memcpy(payload.data(), other.payload.data(),
                std::min<std::size_t>(payloadBytes, payload.size()));
    return *this;
  }
};

class Processor final : public juce::AudioProcessor,
                        private juce::Timer,
                        private juce::AsyncUpdater {
public:
  explicit Processor(Product product);
  ~Processor() override;

  void prepareToPlay(double sampleRate, int maximumBlockSize) override;
  void releaseResources() override {}
  void reset() override;
  [[nodiscard]] bool
  isBusesLayoutSupported(const BusesLayout &layouts) const override;
  [[nodiscard]] bool supportsDoublePrecisionProcessing() const override {
    return false;
  }

  void processBlock(juce::AudioBuffer<float> &audio,
                    juce::MidiBuffer &midi) override;
  void processBlock(juce::AudioBuffer<double> &audio,
                    juce::MidiBuffer &midi) override;

  [[nodiscard]] const juce::String getName() const override;
  [[nodiscard]] bool acceptsMidi() const override { return true; }
  [[nodiscard]] bool producesMidi() const override { return false; }
  [[nodiscard]] bool isMidiEffect() const override { return false; }
  [[nodiscard]] double getTailLengthSeconds() const override;

  [[nodiscard]] bool hasEditor() const override { return true; }
  juce::AudioProcessorEditor *createEditor() override;

  [[nodiscard]] int getNumPrograms() override { return 1; }
  [[nodiscard]] int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  [[nodiscard]] const juce::String getProgramName(int index) override {
    return index == 0 ? juce::String{"Default"} : juce::String{};
  }
  void changeProgramName(int, const juce::String &) override {}

  void getStateInformation(juce::MemoryBlock &destination) override;
  void setStateInformation(const void *data, int byteCount) override;

  void loadFile(const juce::File &file, bool seedDefaults = true);
  [[nodiscard]] bool canExportProject() const;
  [[nodiscard]] std::string saveProjectAs(const juce::File &directory);
  [[nodiscard]] bool
  saveProjectAsAsync(const juce::File &directory,
                     std::function<void(std::string)> completion);
  void bindBufferFile(std::string name, const juce::File &file);
  void clearBuffer(std::string_view name);
  void unload(bool notifyHost = true);
  void requestReload();
  void resetParametersToDefaults();
  void requestUserReset();
  [[nodiscard]] std::string triggerEvent(std::string_view name,
                                         const juce::var &values);
  [[nodiscard]] WorkerStatus workerStatus() const;
  [[nodiscard]] std::uint64_t workerStatusRevision() const;
  [[nodiscard]] RuntimeLogSnapshot runtimeLogSnapshot() const;
  [[nodiscard]] std::uint64_t runtimeLogRevision() const noexcept;
  [[nodiscard]] MidiActivitySnapshot midiActivitySnapshot() const noexcept;
  // Called on the message thread; notes are dispatched by the audio callback.
  void triggerMidiNote(int key, float velocity, bool pressed);
  void releaseKeyboardNotes() noexcept;
  void clearRuntimeLog();
  void setScopeCaptureEnabled(bool enabled) noexcept;
  [[nodiscard]] ScopeRevision scopeRevision() const noexcept;
  [[nodiscard]] ScopeSnapshot scopeSnapshot() const;
  [[nodiscard]] juce::File lastBrowseDirectory() const;
  [[nodiscard]] float slotValue(std::size_t index) const noexcept;
  void beginSlotGesture(std::size_t index);
  void setSlotValue(std::size_t index, float normalized);
  void endSlotGesture(std::size_t index);

  [[nodiscard]] double hostSampleRate() const noexcept {
    return expectedSampleRate_.load(std::memory_order_acquire);
  }
  [[nodiscard]] int hostBlockSize() const noexcept {
    return expectedBlockSize_.load(std::memory_order_acquire);
  }
  [[nodiscard]] Product product() const noexcept { return product_; }

  [[nodiscard]] std::pair<int, int> editorSize() const noexcept;
  void setEditorSize(int width, int height);
  [[nodiscard]] ParamControlLayout paramControlLayout() const noexcept;
  void setParamControlLayout(ParamControlLayout layout);
  [[nodiscard]] juce::String viewState() const;
  void setViewState(const juce::var &state);
  [[nodiscard]] std::uint64_t viewStateRestoreRevision() const noexcept {
    return viewStateRestoreRevision_.load(std::memory_order_acquire);
  }

private:
  friend struct ProcessorTestAccess;
  class SlotParameter;
  static BusesProperties buses();
  static juce::AudioProcessorValueTreeState::ParameterLayout parameters();
  void timerCallback() override;
  void handleAsyncUpdate() override;
  [[nodiscard]] bool refreshSlotParameterInfo();
  void acquireEngine() noexcept;
  // Caller holds preparationMutex_; host audio is suspended or offline.
  [[nodiscard]] std::size_t synchronizeEngine();
  [[nodiscard]] std::size_t applySeedValues();
  void commitSlotValue(std::size_t index, float value);
  void notifySlotValues(std::size_t count);
  [[nodiscard]] std::string
  saveProjectSnapshot(const ProjectExportSnapshot &snapshot,
                      const juce::File &directory);
  void retireActive() noexcept;
  void faultActive() noexcept;
  void drainRuntimeLogs();
  void observeMidiActivity(const juce::MidiMessage &message) noexcept;
  void clearMidiActivity() noexcept;
  [[nodiscard]] static std::optional<MidiEvent>
  convertMidi(const juce::MidiMessage &message, int samplePosition,
              const PreparedEngine &engine, int minimumOffset,
              int frames) noexcept;
  void fallback(juce::AudioBuffer<float> &audio) noexcept;

  Product product_;
  juce::AudioProcessorValueTreeState parameterState_;
  std::array<std::atomic<float> *, slotCount> slotAtomics_{};
  std::array<juce::RangedAudioParameter *, slotCount> slotParameters_{};
  std::array<SlotParameter *, slotCount> presentedSlotParameters_{};

  SpscSlot<PreparedEngine *> replacements_;
  SpscSlot<PreparedEngine *> retirements_;
  std::atomic<bool> deactivateRequested_{};
  std::atomic<bool> resetRequested_{};
  std::atomic<bool> runtimeFaulted_{};
  std::atomic<bool> runtimeRecoveryRequested_{};
  std::atomic<double> expectedSampleRate_{};
  std::atomic<int> expectedBlockSize_{};
  std::atomic<int> configuredBlockSize_{};
  std::unique_ptr<Worker> worker_;
  juce::ThreadPool exportPool_{1};
  std::atomic<bool> exportPending_{};
  PreparedEngine *active_{};
  // Audio-owned quarantine; UI fault reporting may be cleared independently.
  bool activeFaulted_{};
  std::uint64_t offlinePreparedGeneration_{};
  RuntimeLogSink runtimeLogSink_;
  ScopeCapture scopeCapture_;
  SpscQueue<UserEventCommand, 8U> userEvents_;
  UserEventCommand userEventScratch_;
  std::atomic<std::uint64_t> activeLogGeneration_{};
  std::uint64_t consumedLogGeneration_{};
  std::uint64_t consumedLogEpoch_{};
  std::atomic<std::uint64_t> consumedLogActivityRevision_{};
  RuntimeLogCounters consumedLogCounterTotals_{};
  std::atomic<std::uint64_t> runtimeLogRevision_{};
  static constexpr std::size_t midiChannelCount = 16U;
  std::array<std::array<std::atomic<std::uint64_t>, midiNoteWordCount>,
             midiChannelCount>
      midiActiveNotes_{};
  std::atomic<std::uint64_t> midiActivityRevision_{};
  struct KeyboardNoteCommand {
    std::uint64_t generation{};
    std::uint64_t epoch{};
    int key{};
    float velocity{};
  };
  SpscQueue<KeyboardNoteCommand, 256U> keyboardNotes_;
  std::atomic<std::uint64_t> keyboardEpoch_{};
  std::uint64_t consumedKeyboardEpoch_{};
  // Audio-thread ownership, reset whenever the engine generation changes.
  std::array<bool, midiNoteCount> keyboardHeldNotes_{};
  std::uint64_t keyboardGeneration_{};

  std::array<std::vector<float>,
             static_cast<std::size_t>(pluginPassthroughChannels)>
      dryScratch_;
  std::atomic<int> editorWidth_{480};
  std::atomic<int> editorHeight_{720};
  std::atomic<ParamControlLayout> paramControlLayout_{
      ParamControlLayout::sliders};
  std::atomic<std::uint64_t> viewStateRestoreRevision_{};
  std::mutex preparationMutex_;
  mutable std::mutex stateMutex_;
  std::filesystem::path lastBrowseDirectory_;
  juce::String viewState_;
  std::vector<RuntimeLogRecord> runtimeLogRecords_;
  std::size_t runtimeLogBytes_{};
  RuntimeLogCounters runtimeLogCounters_{};
  bool runtimeLogRevealed_{};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
};

} // namespace onda::plugin
