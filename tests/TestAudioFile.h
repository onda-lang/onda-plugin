#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

class TemporaryAudioFile final {
public:
  explicit TemporaryAudioFile(const std::string_view stem)
      : path_(std::filesystem::temp_directory_path() /
              (std::string{stem} + ".flac")) {}

  ~TemporaryAudioFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  bool write(const float left, const float right) const {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    auto fileOutput =
        std::make_unique<juce::FileOutputStream>(juce::File(path_.string()));
    if (!fileOutput->openedOk())
      return false;
    std::unique_ptr<juce::OutputStream> output = std::move(fileOutput);

    juce::FlacAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(48'000.0)
                             .withNumChannels(2)
                             .withBitsPerSample(16);
    auto writer = format.createWriterFor(output, options);
    if (!writer)
      return false;

    juce::AudioBuffer<float> samples(2, 8);
    for (int frame = 0; frame < samples.getNumSamples(); ++frame) {
      samples.setSample(0, frame, left + static_cast<float>(frame) * 0.01F);
      samples.setSample(1, frame, right + static_cast<float>(frame) * 0.01F);
    }
    return writer->writeFromAudioSampleBuffer(samples, 0,
                                              samples.getNumSamples());
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};
