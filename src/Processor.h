#pragma once

#include "Engine.h"
#include "Product.h"
#include "SpscSlot.h"
#include "Worker.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace onda::plugin {

class Processor final : public juce::AudioProcessor, private juce::Timer {
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
  [[nodiscard]] double getTailLengthSeconds() const override { return 0.0; }

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
  void unload();
  void requestReload();
  void resetParametersToDefaults();
  void requestUserReset();
  [[nodiscard]] WorkerStatus workerStatus() const;
  [[nodiscard]] std::uint64_t workerStatusRevision() const;
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
  void setEditorSize(int width, int height) noexcept;

private:
  static BusesProperties buses(Product product);
  static juce::AudioProcessorValueTreeState::ParameterLayout parameters();
  void timerCallback() override;
  void acquireEngine() noexcept;
  void retireActive() noexcept;
  [[nodiscard]] static std::optional<MidiEvent>
  convertMidi(const juce::MidiMessageMetadata &metadata,
              const PreparedEngine &engine, int minimumOffset,
              int frames) noexcept;
  void fallback(juce::AudioBuffer<float> &audio) noexcept;

  Product product_;
  juce::AudioProcessorValueTreeState parameterState_;
  std::array<std::atomic<float> *, slotCount> slotAtomics_{};
  std::array<juce::RangedAudioParameter *, slotCount> slotParameters_{};

  SpscSlot<PreparedEngine *> replacements_;
  SpscSlot<PreparedEngine *> retirements_;
  std::atomic<bool> deactivateRequested_{};
  std::atomic<bool> replacementSeedPending_{};
  std::atomic<bool> resetRequested_{};
  std::atomic<bool> runtimeFaulted_{};
  std::atomic<bool> runtimeRecoveryRequested_{};
  std::atomic<double> expectedSampleRate_{};
  std::atomic<int> expectedBlockSize_{};
  std::unique_ptr<Worker> worker_;
  juce::ThreadPool exportPool_{1};
  std::atomic<bool> exportPending_{};
  PreparedEngine *active_{};

  std::array<std::vector<float>, 2U> dryScratch_;
  std::atomic<int> editorWidth_{480};
  std::atomic<int> editorHeight_{720};
  mutable std::mutex stateMutex_;
  std::filesystem::path lastBrowseDirectory_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
};

} // namespace onda::plugin
