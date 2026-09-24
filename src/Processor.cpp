#include "Processor.h"

#include "Editor.h"
#include "JucePath.h"
#include "ProjectExport.h"
#include "ProjectPath.h"

#include <onda_processor_abi.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_set>

namespace onda::plugin {
namespace {

constexpr std::uint32_t stateMagic = 0x41444e4fU; // ONDA, little-endian.
constexpr int stateVersion = 3;
constexpr int maximumPathBytes = 16 * 1024;
constexpr int maximumBufferNameBytes = 1024;
constexpr int maximumBufferBindings = 1024;
constexpr std::size_t maximumRuntimeLogEntries = 1024U;
constexpr std::size_t maximumRuntimeLogBytes = 256U * 1024U;

static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

[[nodiscard]] std::size_t
runtimeLogPayloadBytes(const RuntimeLogRecord &record) noexcept {
  return record.text.size() + record.sourceFile.size() +
         record.lexicalOwner.size();
}

[[nodiscard]] RuntimeLogCounters
counterDifference(const RuntimeLogCounters &totals,
                  const RuntimeLogCounters &previous) noexcept {
  const auto difference = [](const std::uint64_t total,
                             const std::uint64_t consumed) {
    return total >= consumed ? total - consumed : total;
  };
  return {
      .printOverflow = difference(totals.printOverflow, previous.printOverflow),
      .printTransportDrops =
          difference(totals.printTransportDrops, previous.printTransportDrops),
      .delegateOverflow =
          difference(totals.delegateOverflow, previous.delegateOverflow),
      .delegateTransportDrops = difference(totals.delegateTransportDrops,
                                           previous.delegateTransportDrops),
  };
}

std::optional<juce::String> readBoundedString(juce::MemoryInputStream &stream,
                                              const int maximumBytes) {
  std::string bytes;
  const auto remaining = std::max<juce::int64>(stream.getNumBytesRemaining(),
                                               static_cast<juce::int64>(0));
  bytes.reserve(static_cast<std::size_t>(std::min<juce::int64>(
      static_cast<juce::int64>(maximumBytes), remaining)));
  for (int count = 0; count <= maximumBytes; ++count) {
    if (stream.getNumBytesRemaining() == 0U)
      return std::nullopt;
    const auto byte = stream.readByte();
    if (byte == '\0') {
      if (!juce::CharPointer_UTF8::isValidString(
              bytes.c_str(), static_cast<int>(bytes.size() + 1U))) {
        return std::nullopt;
      }
      return juce::String::fromUTF8(bytes.data(),
                                    static_cast<int>(bytes.size()));
    }
    if (count == maximumBytes)
      return std::nullopt;
    bytes.push_back(byte);
  }
  return std::nullopt;
}

std::string slotId(const std::size_t index) {
  const auto number = static_cast<int>(index + 1U);
  return number < 10 ? "slot0" + std::to_string(number)
                     : "slot" + std::to_string(number);
}

float normalized(const float value) noexcept {
  return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.5F;
}

bool sameSampleRate(const double left, const double right) noexcept {
  return std::bit_cast<std::uint64_t>(left) ==
         std::bit_cast<std::uint64_t>(right);
}

float normalizedPitchBend(const int value) noexcept {
  constexpr auto center = 8192;
  if (value <= center)
    return static_cast<float>(value) / 16384.0F;
  return 0.5F + static_cast<float>(value - center) / 16382.0F;
}

HostContext
captureHostContext(const juce::AudioPlayHead::PositionInfo &position,
                   const bool realtime, const PreparedEngine &engine) noexcept {
  HostContext context;
  context.realtime = realtime;
  if (engine.handlesHostContext(HostContextKind::samplePosition) ||
      engine.handlesHostContext(HostContextKind::timePosition) ||
      engine.handlesHostContext(HostContextKind::musicalPosition)) {
    context.timelinePlaying = position.getIsPlaying();
  }
  if (engine.handlesHostContext(HostContextKind::transport)) {
    context.transport = HostContext::Transport{
        .playing = position.getIsPlaying(),
        .recording = position.getIsRecording(),
        .looping = position.getIsLooping(),
    };
  }
  if (engine.handlesHostContext(HostContextKind::samplePosition)) {
    if (const auto value = position.getTimeInSamples())
      context.samplePosition = *value;
  }
  if (engine.handlesHostContext(HostContextKind::timePosition)) {
    if (const auto value = position.getTimeInSeconds();
        value && std::isfinite(*value)) {
      context.timePosition = *value;
    }
  }
  if (engine.handlesHostContext(HostContextKind::tempo) ||
      engine.handlesHostContext(HostContextKind::musicalPosition)) {
    if (const auto value = position.getBpm();
        value && std::isfinite(*value) && *value > 0.0) {
      context.tempo = *value;
    }
  }
  if (engine.handlesHostContext(HostContextKind::musicalPosition)) {
    if (const auto value = position.getPpqPosition();
        value && std::isfinite(*value)) {
      context.musicalPosition = *value;
    }
  }
  if (engine.handlesHostContext(HostContextKind::barPosition)) {
    if (const auto value = position.getPpqPositionOfLastBarStart();
        value && std::isfinite(*value)) {
      context.barPosition = *value;
    }
  }
  if (engine.handlesHostContext(HostContextKind::timeSignature)) {
    if (const auto value = position.getTimeSignature();
        value && value->numerator > 0 && value->denominator > 0) {
      context.timeSignature = HostContext::TimeSignature{
          .numerator = value->numerator,
          .denominator = value->denominator,
      };
    }
  }
  if (engine.handlesHostContext(HostContextKind::loopRegion)) {
    if (const auto value = position.getLoopPoints();
        value && std::isfinite(value->ppqStart) &&
        std::isfinite(value->ppqEnd)) {
      context.loopRegion = HostContext::LoopRegion{
          .startQuarterNote = value->ppqStart,
          .endQuarterNote = value->ppqEnd,
      };
    }
  }
  return context;
}

void writeProjectImage(juce::OutputStream &stream,
                       const ProjectImage &projectImage) {
  const auto size = projectImage.valid() ? projectImage.bytes->size() : 0U;
  stream.writeInt64(static_cast<std::int64_t>(size));
  if (size > 0U)
    stream.write(projectImage.bytes->data(), size);
}

ProjectImage readProjectImage(juce::MemoryInputStream &stream, bool &valid) {
  const auto byteCount = stream.readInt64();
  if (byteCount == 0)
    return {};
  if (byteCount < 0 || byteCount > stream.getNumBytesRemaining() ||
      byteCount > std::numeric_limits<int>::max() ||
      static_cast<std::uint64_t>(byteCount) >
          std::vector<std::uint8_t>{}.max_size()) {
    valid = false;
    return {};
  }
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(
      static_cast<std::size_t>(byteCount));
  if (stream.read(bytes->data(), static_cast<int>(byteCount)) !=
      static_cast<int>(byteCount)) {
    valid = false;
    return {};
  }
  ProjectImage image{.bytes = std::move(bytes)};
  Diagnostic diagnostic;
  if (!validateProjectImage(image, diagnostic)) {
    valid = false;
    return {};
  }
  return image;
}

} // namespace

class Processor::SlotParameter final : public juce::AudioParameterFloat {
public:
  explicit SlotParameter(const std::size_t index)
      : AudioParameterFloat(juce::ParameterID{slotId(index), 1},
                            slotName(index),
                            juce::NormalisableRange<float>{0.0F, 1.0F}, 0.5F),
        slotName_(slotName(index)) {}

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
    if (mapping && maximumLength <= 8)
      return juce::String(mapping->name).substring(0, maximumLength);
    const auto displayName =
        mapping ? juce::String(mapping->name) + " [" + slotName_ + "]"
                : slotName_;
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
    const auto mapping = mappingSnapshot();
    if (!mapping)
      return juce::AudioProcessorParameter::getDefaultNumParameterSteps();
    if (mapping->type == "bool")
      return 2;
    if (!mapping->stepCount)
      return juce::AudioProcessorParameter::getDefaultNumParameterSteps();
    constexpr auto maximum = std::numeric_limits<int>::max();
    if (*mapping->stepCount >= static_cast<std::int64_t>(maximum - 1))
      return maximum;
    return static_cast<int>(*mapping->stepCount + 1);
  }

  bool isDiscrete() const override {
    const auto mapping = mappingSnapshot();
    return mapping && (mapping->type == "bool" || mapping->stepCount);
  }

  bool isBoolean() const override {
    const auto mapping = mappingSnapshot();
    return mapping && mapping->type == "bool";
  }

  juce::String getText(const float normalizedValue,
                       const int maximumLength) const override {
    const auto mapping = mappingSnapshot();
    if (!mapping)
      return truncate(juce::String(normalizedValue), maximumLength);
    if (mapping->type == "bool")
      return truncate(normalizedValue >= 0.5F ? "On" : "Off", maximumLength);

    const auto parameterDomain = domain(*mapping);
    const auto plain = onda_processor_param_normalized_to_plain(
        &parameterDomain, static_cast<double>(normalizedValue));
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

    const auto parameterDomain = domain(*mapping);
    const auto normalizedValue = onda_processor_param_plain_to_normalized(
        &parameterDomain, text.getDoubleValue());
    return std::isfinite(normalizedValue)
               ? static_cast<float>(std::clamp(normalizedValue, 0.0, 1.0))
               : 0.5F;
  }

private:
  [[nodiscard]] std::shared_ptr<const ParameterMapping>
  mappingSnapshot() const {
    std::lock_guard lock(mappingMutex_);
    return mapping_;
  }

  static juce::String slotName(const std::size_t index) {
    return "Slot " + juce::String(static_cast<int>(index + 1U));
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

  static onda_processor_param_scalar scalar(const std::string_view type) {
    if (type == "f64")
      return ONDA_PROCESSOR_PARAM_SCALAR_F64;
    if (type == "i32")
      return ONDA_PROCESSOR_PARAM_SCALAR_I32;
    if (type == "i64")
      return ONDA_PROCESSOR_PARAM_SCALAR_I64;
    return ONDA_PROCESSOR_PARAM_SCALAR_F32;
  }

  static onda_processor_param_domain domain(const ParameterMapping &mapping) {
    return {
        .minimum = mapping.rangeMin,
        .maximum = mapping.rangeMax,
        .step = mapping.step.value_or(0.0),
        .curve = mapping.curve.value_or(0.0),
        .step_count = static_cast<std::uint32_t>(mapping.stepCount.value_or(0)),
        .scale = mapping.scale == "log" ? ONDA_PROCESSOR_PARAM_SCALE_LOG
                                        : ONDA_PROCESSOR_PARAM_SCALE_LINEAR,
        .scalar = scalar(mapping.type),
        .has_curve = static_cast<std::uint8_t>(mapping.curve.has_value()),
        .unit = nullptr,
    };
  }

  const juce::String slotName_;
  mutable std::mutex mappingMutex_;
  std::shared_ptr<const ParameterMapping> mapping_;
};

Processor::Processor(const Product product)
    : AudioProcessor(buses()), product_(product),
      parameterState_(*this, nullptr, "OndaSlots", parameters()) {
  for (std::size_t index = 0; index < slotCount; ++index) {
    const auto id = slotId(index);
    slotAtomics_[index] = parameterState_.getRawParameterValue(id);
    slotParameters_[index] = parameterState_.getParameter(id);
    presentedSlotParameters_[index] =
        dynamic_cast<SlotParameter *>(slotParameters_[index]);
    jassert(slotAtomics_[index] != nullptr &&
            slotParameters_[index] != nullptr &&
            presentedSlotParameters_[index] != nullptr);
  }
  worker_ = std::make_unique<Worker>(
      product_, replacements_, retirements_, deactivateRequested_,
      Worker::BuildFunction{}, nullptr,
      [this] {
        ParameterValues values;
        for (std::size_t index = 0; index < slotCount; ++index)
          values[index] = slotValue(index);
        return values;
      },
      Worker::ProjectBuildFunction{}, [this] { triggerAsyncUpdate(); });
  startTimerHz(20);
}

Processor::~Processor() {
  stopTimer();
  exportPool_.removeAllJobs(false, -1);
  worker_.reset();
  cancelPendingUpdate();
  delete replacements_.tryPop();
  delete retirements_.tryPop();
  delete active_;
}

juce::AudioProcessor::BusesProperties Processor::buses() {
  BusesProperties result;
  if constexpr (pluginInputChannels > 0) {
    result = result.withInput(
        "Input",
        juce::AudioChannelSet::canonicalChannelSet(pluginInputChannels), true);
  }
  return result.withOutput(
      "Output",
      juce::AudioChannelSet::canonicalChannelSet(pluginOutputChannels), true);
}

juce::AudioProcessorValueTreeState::ParameterLayout Processor::parameters() {
  juce::AudioProcessorValueTreeState::ParameterLayout layout;
  for (std::size_t index = 0; index < slotCount; ++index)
    layout.add(std::make_unique<SlotParameter>(index));
  return layout;
}

void Processor::prepareToPlay(const double sampleRate,
                              const int maximumBlockSize) {
  std::unique_lock preparationLock(preparationMutex_);
  const auto safeBlockSize = std::max(maximumBlockSize, 1);
  const auto changed =
      !sameSampleRate(sampleRate,
                      expectedSampleRate_.load(std::memory_order_acquire)) ||
      safeBlockSize != expectedBlockSize_.load(std::memory_order_acquire);
  expectedSampleRate_.store(sampleRate, std::memory_order_release);
  expectedBlockSize_.store(safeBlockSize, std::memory_order_release);
  for (auto &channel : dryScratch_)
    channel.resize(static_cast<std::size_t>(safeBlockSize));
  if (changed) {
    deactivateRequested_.store(true, std::memory_order_release);
  }
  clearMidiActivity();
  worker_->configure(sampleRate, safeBlockSize);
  configuredBlockSize_.store(safeBlockSize, std::memory_order_release);
  const auto seeded = synchronizeEngine();
  preparationLock.unlock();
  notifySlotValues(seeded);
}

std::size_t Processor::synchronizeEngine() {
  // Called with audio suspended during host preparation, or from an offline
  // callback. Neither path needs a message-loop tick to finish preparation.
  offlinePreparedGeneration_ = worker_->waitForPreparation();
  delete retirements_.tryPop();
  acquireEngine();
  return applySeedValues();
}

double Processor::getTailLengthSeconds() const {
  // Arbitrary Onda programs can sustain indefinitely. JUCE maps infinity to
  // VST3's unknown/infinite tail; an unloaded plugin has no tail.
  return worker_->hasPreparedEngine() ? std::numeric_limits<double>::infinity()
                                      : 0.0;
}

void Processor::reset() {
  clearMidiActivity();
  if (runtimeFaulted_.load(std::memory_order_acquire))
    runtimeRecoveryRequested_.store(true, std::memory_order_release);
  else
    resetRequested_.store(true, std::memory_order_release);
}

bool Processor::isBusesLayoutSupported(const BusesLayout &layouts) const {
  const auto expectedOutput =
      juce::AudioChannelSet::canonicalChannelSet(pluginOutputChannels);
  if (layouts.getMainOutputChannelSet() != expectedOutput)
    return false;
  if constexpr (pluginInputChannels == 0)
    return layouts.getMainInputChannelSet().isDisabled();
  return layouts.getMainInputChannelSet() ==
         juce::AudioChannelSet::canonicalChannelSet(pluginInputChannels);
}

const juce::String Processor::getName() const {
  return juce::String(productName(product_).data());
}

void Processor::retireActive() noexcept {
  if (active_ != nullptr && retirements_.tryPush(active_)) {
    active_ = nullptr;
    worker_->setActiveGeneration(0);
    clearMidiActivity();
  }
}

void Processor::acquireEngine() noexcept {
  if (activeFaulted_)
    retireActive();
  if (deactivateRequested_.load(std::memory_order_acquire)) {
    retireActive();
    // The worker owns superseded queued engines. Do not consume a replacement
    // here: publication can race a host lifecycle change.
    return;
  }

  if (replacements_.empty() || !retirements_.empty())
    return;

  auto *replacement = replacements_.tryPop();
  if (replacement == nullptr)
    return;
  if (replacement->buildGeneration() != worker_->requestGeneration()) {
    static_cast<void>(retirements_.tryPush(replacement));
    return;
  }
  if (active_ != nullptr) {
    const auto retired = retirements_.tryPush(active_);
    jassert(retired);
    juce::ignoreUnused(retired);
  }
  static_cast<void>(runtimeLogSink_.beginEpoch());
  activeLogGeneration_.store(replacement->buildGeneration(),
                             std::memory_order_release);
  replacement->attachLogSink(runtimeLogSink_);
  active_ = replacement;
  activeFaulted_ = false;
  worker_->setActiveGeneration(replacement->buildGeneration());
  clearMidiActivity();
  scopeCapture_.requestReset();
  runtimeFaulted_.store(false, std::memory_order_release);
}

void Processor::faultActive() noexcept {
  // A late failure belongs to this engine, not a concurrently published one.
  activeFaulted_ = true;
  worker_->setActiveGeneration(0);
  clearMidiActivity();
  runtimeFaulted_.store(true, std::memory_order_release);
}

std::optional<MidiEvent>
Processor::convertMidi(const juce::MidiMessage &message,
                       const int samplePosition, const PreparedEngine &engine,
                       const int minimumOffset, const int frames) noexcept {
  MidiEvent event;
  if (message.isNoteOn())
    event.kind = MidiKind::noteOn;
  else if (message.isNoteOff())
    event.kind = MidiKind::noteOff;
  else if (message.isAftertouch())
    event.kind = MidiKind::polyPressure;
  else if (message.isPitchWheel())
    event.kind = MidiKind::pitchBend;
  else if (message.isChannelPressure())
    event.kind = MidiKind::channelPressure;
  else if (message.isController())
    event.kind = MidiKind::controlChange;
  else if (message.isProgramChange())
    event.kind = MidiKind::programChange;
  else
    return std::nullopt;

  if (!engine.handlesMidi(event.kind))
    return std::nullopt;

  event.channel = std::max(message.getChannel() - 1, 0);
  switch (event.kind) {
  case MidiKind::noteOn:
    event.keyOrController = message.getNoteNumber();
    event.value = normalized(message.getFloatVelocity());
    break;
  case MidiKind::noteOff:
    event.keyOrController = message.getNoteNumber();
    event.value = normalized(message.getFloatVelocity());
    break;
  case MidiKind::polyPressure:
    event.keyOrController = message.getNoteNumber();
    event.value =
        normalized(static_cast<float>(message.getAfterTouchValue()) / 127.0F);
    break;
  case MidiKind::pitchBend:
    event.value = normalizedPitchBend(message.getPitchWheelValue());
    break;
  case MidiKind::channelPressure:
    event.value = normalized(
        static_cast<float>(message.getChannelPressureValue()) / 127.0F);
    break;
  case MidiKind::controlChange:
    event.keyOrController = message.getControllerNumber();
    event.value =
        normalized(static_cast<float>(message.getControllerValue()) / 127.0F);
    break;
  case MidiKind::programChange:
    event.keyOrController = message.getProgramChangeNumber();
    break;
  case MidiKind::count:
    return std::nullopt;
  }
  event.sampleOffset = static_cast<std::uint32_t>(
      std::clamp(samplePosition, minimumOffset, frames) - minimumOffset);
  return event;
}

void Processor::observeMidiActivity(const juce::MidiMessage &message) noexcept {
  const auto channel = message.getChannel() - 1;
  if (channel < 0 || channel >= static_cast<int>(midiChannelCount))
    return;

  if (message.isController() && (message.getControllerNumber() == 120 ||
                                 message.getControllerNumber() == 123)) {
    auto changed = false;
    for (auto &word : midiActiveNotes_[static_cast<std::size_t>(channel)])
      changed = word.exchange(0U, std::memory_order_relaxed) != 0U || changed;
    if (changed)
      midiActivityRevision_.fetch_add(1U, std::memory_order_release);
    return;
  }

  const auto noteOn = message.isNoteOn();
  if (!noteOn && !message.isNoteOff())
    return;
  const auto note = message.getNoteNumber();
  if (note < 0 || note >= static_cast<int>(midiNoteCount)) {
    return;
  }
  const auto index = static_cast<std::size_t>(note) / 64U;
  const auto bit = std::uint64_t{1} << (static_cast<std::size_t>(note) % 64U);
  auto &word = midiActiveNotes_[static_cast<std::size_t>(channel)][index];
  const auto previous = noteOn
                            ? word.fetch_or(bit, std::memory_order_relaxed)
                            : word.fetch_and(~bit, std::memory_order_relaxed);
  if (((previous & bit) != 0U) != noteOn)
    midiActivityRevision_.fetch_add(1U, std::memory_order_release);
}

void Processor::clearMidiActivity() noexcept {
  auto changed = false;
  for (auto &channel : midiActiveNotes_) {
    for (auto &word : channel)
      changed = word.exchange(0U, std::memory_order_relaxed) != 0U || changed;
  }
  if (changed)
    midiActivityRevision_.fetch_add(1U, std::memory_order_release);
}

void Processor::fallback(juce::AudioBuffer<float> &audio) noexcept {
  const auto passthroughChannels =
      product_ == Product::effect ? pluginPassthroughChannels : 0;
  for (int channel = passthroughChannels; channel < pluginOutputChannels;
       ++channel)
    audio.clear(channel, 0, audio.getNumSamples());
}

void Processor::processBlock(juce::AudioBuffer<float> &audio,
                             juce::MidiBuffer &midi) {
  juce::ScopedNoDenormals noDenormals;
  const auto frames = audio.getNumSamples();
  if (isNonRealtime()) {
    if (runtimeRecoveryRequested_.exchange(false, std::memory_order_acq_rel))
      worker_->requestRebuild();
    const auto required =
        std::max(frames, expectedBlockSize_.load(std::memory_order_acquire));
    if (required != configuredBlockSize_.load(std::memory_order_acquire))
      prepareToPlay(expectedSampleRate_.load(std::memory_order_acquire),
                    required);
    else if (offlinePreparedGeneration_ != worker_->requestGeneration()) {
      std::unique_lock preparationLock(preparationMutex_);
      const auto seeded = synchronizeEngine();
      preparationLock.unlock();
      notifySlotValues(seeded);
    }
  }
  const auto maximum = expectedBlockSize_.load(std::memory_order_acquire);
  if (frames < 0 || frames > maximum || maximum <= 0) {
    fallback(audio);
    if (frames > maximum && frames > 0) {
      deactivateRequested_.store(true, std::memory_order_release);
      expectedBlockSize_.store(frames, std::memory_order_release);
    }
    return;
  }

  acquireEngine();
  if (deactivateRequested_.load(std::memory_order_acquire) ||
      active_ == nullptr || activeFaulted_ ||
      !sameSampleRate(active_->sampleRate(),
                      expectedSampleRate_.load(std::memory_order_acquire)) ||
      active_->blockSize() != maximum) {
    fallback(audio);
    return;
  }

  HostContext hostContext;
  hostContext.realtime = !isNonRealtime();
  if (active_->needsPositionInfo()) {
    if (const auto *currentPlayHead = getPlayHead()) {
      if (const auto position = currentPlayHead->getPosition()) {
        hostContext =
            captureHostContext(*position, hostContext.realtime, *active_);
      }
    }
  }

  if (resetRequested_.exchange(false, std::memory_order_acq_rel) &&
      !active_->reset(slotAtomics_)) {
    faultActive();
    fallback(audio);
    return;
  }

  if (!active_->beginHostCallback(slotAtomics_)) {
    faultActive();
    fallback(audio);
    return;
  }

  while (userEvents_.tryPop(userEventScratch_)) {
    if (userEventScratch_.generation != active_->buildGeneration())
      continue;
    if (userEventScratch_.payloadBytes > userEventScratch_.payload.size()) {
      faultActive();
      fallback(audio);
      return;
    }
    const auto result = active_->triggerEvent(
        userEventScratch_.eventIndex,
        {userEventScratch_.payload.data(),
         static_cast<std::size_t>(userEventScratch_.payloadBytes)},
        slotAtomics_, hostContext);
    if (result == EventTriggerResult::inputRejected)
      continue;
    if (result == EventTriggerResult::runtimeFailure) {
      faultActive();
      fallback(audio);
      return;
    }
  }

  if (product_ == Product::effect) {
    for (int channel = 0; channel < static_cast<int>(dryScratch_.size());
         ++channel) {
      std::copy_n(audio.getReadPointer(channel), frames,
                  dryScratch_[static_cast<std::size_t>(channel)].data());
    }
  }

  std::array<float *, static_cast<std::size_t>(pluginInputChannels)> inputs{};
  for (int channel = 0; channel < pluginInputChannels; ++channel)
    inputs[static_cast<std::size_t>(channel)] = audio.getWritePointer(channel);
  std::array<float *, static_cast<std::size_t>(pluginOutputChannels)> outputs{};
  for (int channel = 0; channel < pluginOutputChannels; ++channel)
    outputs[static_cast<std::size_t>(channel)] = audio.getWritePointer(channel);
  const auto processRange =
      [this, &inputs, &outputs,
       &hostContext](const int offset, const int count,
                     const std::span<const MidiEvent> events) noexcept {
        std::array<float *, static_cast<std::size_t>(pluginInputChannels)>
            shiftedInputs{};
        for (int channel = 0; channel < pluginInputChannels; ++channel) {
          shiftedInputs[static_cast<std::size_t>(channel)] =
              inputs[static_cast<std::size_t>(channel)] + offset;
        }
        std::array<float *, static_cast<std::size_t>(pluginOutputChannels)>
            shiftedOutputs{};
        for (int channel = 0; channel < pluginOutputChannels; ++channel) {
          shiftedOutputs[static_cast<std::size_t>(channel)] =
              outputs[static_cast<std::size_t>(channel)] + offset;
        }
        return active_->process(shiftedInputs.data(), shiftedOutputs.data(),
                                count, events, slotAtomics_, hostContext,
                                offset);
      };

  if (keyboardGeneration_ != active_->buildGeneration()) {
    keyboardHeldNotes_.fill(false);
    keyboardGeneration_ = active_->buildGeneration();
  }
  const auto dispatchKeyboardNote = [&](const int key, const float velocity) {
    const MidiEvent event{velocity > 0.0F ? MidiKind::noteOn
                                          : MidiKind::noteOff,
                          0, 0, key, velocity};
    if (!processRange(0, 0, std::span<const MidiEvent>{&event, 1U}))
      return false;
    keyboardHeldNotes_[static_cast<std::size_t>(key)] = velocity > 0.0F;
    return true;
  };
  auto succeeded = true;
  const auto epoch = keyboardEpoch_.load(std::memory_order_acquire);
  if (consumedKeyboardEpoch_ != epoch) {
    for (std::size_t key = 0; key < keyboardHeldNotes_.size() && succeeded;
         ++key) {
      if (keyboardHeldNotes_[key])
        succeeded = dispatchKeyboardNote(static_cast<int>(key), 0.0F);
    }
    consumedKeyboardEpoch_ = epoch;
  }
  KeyboardNoteCommand keyboardNote;
  // Bound callback work even if the producer continues submitting notes.
  for (std::size_t count = 0;
       count < 256U && keyboardNotes_.tryPop(keyboardNote); ++count) {
    if (keyboardNote.generation == keyboardGeneration_ &&
        keyboardNote.epoch == epoch && succeeded)
      succeeded = dispatchKeyboardNote(keyboardNote.key, keyboardNote.velocity);
  }
  auto position = 0;
  for (const auto metadata : midi) {
    if (!succeeded)
      break;
    // Only channel messages are supported. Reject SysEx/system messages before
    // getMessage(), which allocates for packets exceeding JUCE's inline
    // storage.
    if (metadata.data == nullptr || metadata.numBytes < 2 ||
        metadata.numBytes > 3)
      continue;
    const auto status = metadata.data[0];
    if (status < 0x80 || status >= 0xf0)
      continue;
    const auto requiredBytes = (status & 0xe0) == 0xc0 ? 2 : 3;
    if (metadata.numBytes != requiredBytes || metadata.data[1] >= 0x80 ||
        (requiredBytes == 3 && metadata.data[2] >= 0x80))
      continue;
    const auto message = metadata.getMessage();
    observeMidiActivity(message);
    auto event = convertMidi(message, metadata.samplePosition, *active_,
                             position, frames);
    if (!event)
      continue;
    const auto eventOffset = position + static_cast<int>(event->sampleOffset);
    if (eventOffset > position) {
      if (!processRange(position, eventOffset - position, {})) {
        succeeded = false;
        break;
      }
      position = eventOffset;
    }
    event->sampleOffset = 0;
    if (!processRange(position, 0, std::span<const MidiEvent>{&*event, 1U})) {
      succeeded = false;
      break;
    }
  }
  if (succeeded && position < frames)
    succeeded = processRange(position, frames - position, {});
  if (!succeeded) {
    faultActive();
    fallback(audio);
    if (product_ == Product::effect) {
      for (int channel = 0; channel < static_cast<int>(dryScratch_.size());
           ++channel) {
        std::copy_n(dryScratch_[static_cast<std::size_t>(channel)].data(),
                    frames, audio.getWritePointer(channel));
      }
    }
  }
  scopeCapture_.push(outputs, frames);
}

void Processor::processBlock(juce::AudioBuffer<double> &audio,
                             juce::MidiBuffer &) {
  const auto passthroughChannels =
      product_ == Product::effect ? pluginPassthroughChannels : 0;
  for (int channel = passthroughChannels; channel < pluginOutputChannels;
       ++channel)
    audio.clear(channel, 0, audio.getNumSamples());
}

juce::AudioProcessorEditor *Processor::createEditor() {
  return new Editor(*this);
}

void Processor::loadFile(const juce::File &file, const bool seedDefaults) {
  if (file.existsAsFile()) {
    const auto path = pathFromJuce(file.getFullPathName());
    {
      std::lock_guard lock(stateMutex_);
      lastBrowseDirectory_ =
          pathFromJuce(file.getParentDirectory().getFullPathName());
    }
    if (isOndaProjectPath(path))
      worker_->loadWithBufferBindings(path, {}, seedDefaults);
    else
      worker_->load(path, seedDefaults);
  }
}

bool Processor::canExportProject() const {
  return worker_->projectExportSnapshot().has_value();
}

std::string Processor::saveProjectAs(const juce::File &directory) {
  return saveProjectSnapshot(
      worker_->projectExportSnapshot().value_or(ProjectExportSnapshot{}),
      directory);
}

std::string
Processor::saveProjectSnapshot(const ProjectExportSnapshot &snapshot,
                               const juce::File &directory) {
  if (!snapshot.projectImage.valid())
    return "The current Onda project is not ready to export";
  try {
    auto exported = exportProject(snapshot.projectImage,
                                  pathFromJuce(directory.getFullPathName()));
    if (!exported)
      return exported.error;
    if (!worker_->relinkExport(snapshot, exported.projectFile))
      return "Project exported, but not loaded because the current project "
             "changed";
    {
      std::lock_guard lock(stateMutex_);
      lastBrowseDirectory_ = exported.projectFile.parent_path();
    }
    return {};
  } catch (const std::bad_alloc &) {
    return "Insufficient memory to export the Onda project";
  } catch (const std::exception &exception) {
    return std::string{"Failed to export the Onda project: "} +
           exception.what();
  }
}

bool Processor::saveProjectAsAsync(
    const juce::File &directory,
    std::function<void(std::string)> completionHandler) {
  if (exportPending_.exchange(true, std::memory_order_acq_rel))
    return false;
  try {
    const auto snapshot =
        worker_->projectExportSnapshot().value_or(ProjectExportSnapshot{});
    exportPool_.addJob(
        [this, snapshot, directory,
         callback = std::move(completionHandler)]() mutable {
          auto saveResult = saveProjectSnapshot(snapshot, directory);
          exportPending_.store(false, std::memory_order_release);
          if (callback) {
            juce::MessageManager::callAsync(
                [deliveredCallback = std::move(callback),
                 deliveredResult = std::move(saveResult)]() mutable {
                  deliveredCallback(std::move(deliveredResult));
                });
          }
        });
  } catch (...) {
    exportPending_.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

void Processor::bindBufferFile(std::string name, const juce::File &file) {
  if (name.empty() || file.getFullPathName().isEmpty())
    return;
  {
    std::lock_guard lock(stateMutex_);
    lastBrowseDirectory_ =
        pathFromJuce(file.getParentDirectory().getFullPathName());
  }
  worker_->bindBufferFile(std::move(name),
                          pathFromJuce(file.getFullPathName()));
}

void Processor::clearBuffer(const std::string_view name) {
  worker_->clearBuffer(name);
}

void Processor::unload(const bool notifyHost) {
  runtimeFaulted_.store(false, std::memory_order_release);
  runtimeRecoveryRequested_.store(false, std::memory_order_release);
  resetRequested_.store(false, std::memory_order_release);
  activeLogGeneration_.store(0U, std::memory_order_release);
  clearMidiActivity();
  clearRuntimeLog();
  worker_->unload(notifyHost);
}

void Processor::requestReload() { worker_->requestRebuild(); }

void Processor::resetParametersToDefaults() {
  const auto status = worker_->status();
  const auto mapped = std::min(status.mappings.size(), slotCount);
  for (std::size_t index = 0; index < slotCount; ++index) {
    const auto value =
        index < mapped
            ? static_cast<float>(status.mappings[index].defaultNormalized)
            : 0.5F;
    beginSlotGesture(index);
    setSlotValue(index, value);
    endSlotGesture(index);
  }
}

void Processor::requestUserReset() {
  resetParametersToDefaults();
  reset();
}

std::string Processor::triggerEvent(const std::string_view name,
                                    const juce::var &values) {
  const auto status = worker_->status();
  if (!status.active || status.engineGeneration == 0)
    return "No active Onda program";
  const auto event =
      std::ranges::find(status.events, name, &EventMapping::name);
  if (event == status.events.end())
    return "Unknown Onda event '" + std::string{name} + "'";
  const auto *arguments = values.getArray();
  if (arguments == nullptr ||
      static_cast<std::size_t>(arguments->size()) != event->parameters.size()) {
    return "Event '" + event->name + "' has an invalid argument count";
  }

  UserEventCommand command;
  command.generation = status.engineGeneration;
  command.eventIndex = event->index;
  std::size_t payloadBytes{};
  std::string payloadError;
  if (!encodePayload(event->schema, values, command.payload, payloadBytes,
                     payloadError))
    return "Event '" + event->name +
           "' has an invalid payload: " + payloadError;
  command.payloadBytes = static_cast<std::uint32_t>(payloadBytes);
  if (!userEvents_.tryPush(command))
    return "The event queue is full";
  return {};
}

WorkerStatus Processor::workerStatus() const {
  auto status = worker_->status();
  const auto faulted = runtimeFaulted_.load(std::memory_order_acquire);
  status.revision =
      (status.revision << 1U) | static_cast<std::uint64_t>(faulted);
  if (faulted) {
    status.message = "Onda runtime safety check failed; processing is bypassed";
    status.compiling = false;
    status.active = false;
  }
  return status;
}

std::uint64_t Processor::workerStatusRevision() const {
  const auto revision = worker_->statusRevision();
  const auto faulted = runtimeFaulted_.load(std::memory_order_acquire);
  return (revision << 1U) | static_cast<std::uint64_t>(faulted);
}

RuntimeLogSnapshot Processor::runtimeLogSnapshot() const {
  std::lock_guard lock(stateMutex_);
  return {
      .records = runtimeLogRecords_,
      .counters = runtimeLogCounters_,
      .revealed = runtimeLogRevealed_,
  };
}

std::uint64_t Processor::runtimeLogRevision() const noexcept {
  return runtimeLogRevision_.load(std::memory_order_acquire);
}

void Processor::triggerMidiNote(const int key, const float velocity,
                                const bool pressed) {
  if (product_ != Product::instrument || key < 0 ||
      key >= static_cast<int>(midiNoteCount) || !std::isfinite(velocity))
    return;
  const auto status = workerStatus();
  if (!status.active || !status.midi.noteOn || !status.midi.noteOff)
    return;
  if (!keyboardNotes_.tryPush(
          {status.engineGeneration,
           keyboardEpoch_.load(std::memory_order_acquire), key,
           pressed ? std::clamp(velocity, 0.0F, 1.0F) : 0.0F}))
    releaseKeyboardNotes();
}

void Processor::releaseKeyboardNotes() noexcept {
  keyboardEpoch_.fetch_add(1U, std::memory_order_release);
}

MidiActivitySnapshot Processor::midiActivitySnapshot() const noexcept {
  MidiActivitySnapshot snapshot;
  for (;;) {
    const auto before = midiActivityRevision_.load(std::memory_order_acquire);
    snapshot.notes = {};
    for (const auto &channel : midiActiveNotes_) {
      for (std::size_t index = 0; index < snapshot.notes.size(); ++index) {
        snapshot.notes[index] |= channel[index].load(std::memory_order_relaxed);
      }
    }
    const auto after = midiActivityRevision_.load(std::memory_order_acquire);
    if (before == after) {
      snapshot.revision = after;
      return snapshot;
    }
  }
}

void Processor::clearRuntimeLog() {
  {
    std::lock_guard lock(stateMutex_);
    consumedLogEpoch_ = runtimeLogSink_.beginEpoch();
    consumedLogCounterTotals_ = {};
    runtimeLogRecords_.clear();
    runtimeLogBytes_ = 0U;
    runtimeLogCounters_ = {};
  }
  runtimeLogRevision_.fetch_add(1U, std::memory_order_release);
}

void Processor::setScopeCaptureEnabled(const bool enabled) noexcept {
  scopeCapture_.setEnabled(enabled);
}

ScopeRevision Processor::scopeRevision() const noexcept {
  return scopeCapture_.revision();
}

ScopeSnapshot Processor::scopeSnapshot() const {
  return scopeCapture_.snapshot();
}

juce::File Processor::lastBrowseDirectory() const {
  std::lock_guard lock(stateMutex_);
  return lastBrowseDirectory_.empty()
             ? juce::File{}
             : juce::File{pathToJuce(lastBrowseDirectory_)};
}

float Processor::slotValue(const std::size_t index) const noexcept {
  return index < slotCount
             ? slotAtomics_[index]->load(std::memory_order_relaxed)
             : 0.5F;
}

void Processor::beginSlotGesture(const std::size_t index) {
  if (index < slotCount)
    slotParameters_[index]->beginChangeGesture();
}

void Processor::setSlotValue(const std::size_t index, const float value) {
  if (index < slotCount)
    slotParameters_[index]->setValueNotifyingHost(normalized(value));
}

void Processor::endSlotGesture(const std::size_t index) {
  if (index < slotCount)
    slotParameters_[index]->endChangeGesture();
}

std::pair<int, int> Processor::editorSize() const noexcept {
  return {editorWidth_.load(std::memory_order_relaxed),
          editorHeight_.load(std::memory_order_relaxed)};
}

void Processor::setEditorSize(const int width, const int height) {
  const auto clampedWidth = std::clamp(width, 360, 1600);
  const auto clampedHeight = std::clamp(height, 480, 1400);
  const auto widthChanged =
      editorWidth_.exchange(clampedWidth, std::memory_order_relaxed) !=
      clampedWidth;
  const auto heightChanged =
      editorHeight_.exchange(clampedHeight, std::memory_order_relaxed) !=
      clampedHeight;
  if (widthChanged || heightChanged) {
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                          .withNonParameterStateChanged(true));
  }
}

ParamControlLayout Processor::paramControlLayout() const noexcept {
  return paramControlLayout_.load(std::memory_order_relaxed);
}

void Processor::setParamControlLayout(const ParamControlLayout layout) {
  if (paramControlLayout_.exchange(layout, std::memory_order_relaxed) !=
      layout) {
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                          .withNonParameterStateChanged(true));
  }
}

void Processor::drainRuntimeLogs() {
  const auto activityRevision = runtimeLogSink_.activityRevision();
  const auto observedGeneration =
      activeLogGeneration_.load(std::memory_order_acquire);
  if (activityRevision ==
          consumedLogActivityRevision_.load(std::memory_order_acquire) &&
      observedGeneration == consumedLogGeneration_) {
    return;
  }

  struct StagedRecord {
    std::uint64_t generation{};
    std::uint64_t epoch{};
    RuntimeLogRecord record;
  };

  std::vector<StagedRecord> records;
  RuntimeLogEntry entry;
  for (std::size_t count = 0U;
       count + 1U < runtimeLogQueueCapacity && runtimeLogSink_.tryPop(entry);
       ++count) {
    if (records.empty())
      records.reserve(runtimeLogQueueCapacity - 1U);
    const auto textBytes =
        std::min<std::size_t>(entry.textBytes, entry.text.size());
    const auto sourceFileBytes =
        std::min<std::size_t>(entry.sourceFileBytes, entry.sourceFile.size());
    const auto lexicalOwnerBytes = std::min<std::size_t>(
        entry.lexicalOwnerBytes, entry.lexicalOwner.size());
    records.push_back({
        .generation = entry.generation,
        .epoch = entry.epoch,
        .record =
            {
                .kind = entry.kind,
                .text = std::string(entry.text.data(), textBytes),
                .sourceFile =
                    std::string(entry.sourceFile.data(), sourceFileBytes),
                .lexicalOwner =
                    std::string(entry.lexicalOwner.data(), lexicalOwnerBytes),
                .line = entry.line,
            },
    });
  }

  const auto counterEpoch = runtimeLogSink_.epoch();
  const auto counterTotals = runtimeLogSink_.counterTotals(counterEpoch);
  const auto generation = activeLogGeneration_.load(std::memory_order_acquire);
  const auto epoch = runtimeLogSink_.epoch();
  const auto countersStable = counterEpoch == epoch;
  std::lock_guard lock(stateMutex_);
  if (activeLogGeneration_.load(std::memory_order_acquire) != generation ||
      runtimeLogSink_.epoch() != epoch)
    return;
  const auto counters =
      countersStable
          ? counterDifference(counterTotals, consumedLogEpoch_ == epoch
                                                 ? consumedLogCounterTotals_
                                                 : RuntimeLogCounters{})
          : RuntimeLogCounters{};
  std::erase_if(records, [generation, epoch](const StagedRecord &record) {
    return generation == 0U || record.generation != generation ||
           record.epoch != epoch;
  });

  const auto hasCounters =
      counters.printOverflow != 0U || counters.printTransportDrops != 0U ||
      counters.delegateOverflow != 0U || counters.delegateTransportDrops != 0U;
  const auto hasRecords = !records.empty();
  const auto generationChanged = generation != consumedLogGeneration_;
  if (!generationChanged && !hasRecords && !hasCounters) {
    consumedLogActivityRevision_.store(activityRevision,
                                       std::memory_order_release);
    return;
  }

  if (generationChanged) {
    runtimeLogRecords_.clear();
    runtimeLogBytes_ = 0U;
    runtimeLogCounters_ = {};
    runtimeLogRevealed_ = false;
    consumedLogGeneration_ = generation;
  }
  if (generation != 0U) {
    for (auto &record : records) {
      runtimeLogBytes_ += runtimeLogPayloadBytes(record.record);
      runtimeLogRecords_.push_back(std::move(record.record));
    }
    auto discarded = std::size_t{};
    while (discarded < runtimeLogRecords_.size() &&
           (runtimeLogRecords_.size() - discarded > maximumRuntimeLogEntries ||
            runtimeLogBytes_ > maximumRuntimeLogBytes)) {
      const auto &record = runtimeLogRecords_[discarded];
      runtimeLogBytes_ -= runtimeLogPayloadBytes(record);
      auto &dropCounter = record.kind == RuntimeLogKind::print
                              ? runtimeLogCounters_.printTransportDrops
                              : runtimeLogCounters_.delegateTransportDrops;
      addSaturated(dropCounter, 1U);
      ++discarded;
    }
    if (discarded != 0U) {
      runtimeLogRecords_.erase(runtimeLogRecords_.begin(),
                               runtimeLogRecords_.begin() +
                                   static_cast<std::ptrdiff_t>(discarded));
    }
    addSaturated(runtimeLogCounters_.printOverflow, counters.printOverflow);
    addSaturated(runtimeLogCounters_.printTransportDrops,
                 counters.printTransportDrops);
    addSaturated(runtimeLogCounters_.delegateOverflow,
                 counters.delegateOverflow);
    addSaturated(runtimeLogCounters_.delegateTransportDrops,
                 counters.delegateTransportDrops);
    runtimeLogRevealed_ = runtimeLogRevealed_ || hasRecords || hasCounters;
  }
  if (countersStable) {
    consumedLogEpoch_ = epoch;
    consumedLogCounterTotals_ = counterTotals;
  }
  consumedLogActivityRevision_.store(activityRevision,
                                     std::memory_order_release);
  runtimeLogRevision_.fetch_add(1U, std::memory_order_release);
}

void Processor::commitSlotValue(const std::size_t index, const float value) {
  const auto committed = normalized(value);
  slotParameters_[index]->setValue(committed);
  // DSP and worker snapshots read the APVTS atomics, whose listener update
  // otherwise waits until notification. Keep these private mirrors in sync.
  slotAtomics_[index]->store(committed, std::memory_order_relaxed);
}

void Processor::notifySlotValues(const std::size_t count) {
  // Host callbacks may synchronously save/restore state or prepare the engine.
  // Read the current value so reentry cannot replay stale committed values.
  for (std::size_t index = 0; index < count; ++index)
    slotParameters_[index]->sendValueChangedMessageToListeners(
        slotParameters_[index]->getValue());
}

std::size_t Processor::applySeedValues() {
  if (auto seed = worker_->takeSeedValues()) {
    // Commit every slot before exposing the completed seed to host callbacks.
    for (std::size_t index = 0; index < seed->count; ++index)
      commitSlotValue(index, seed->values[index]);
    worker_->finishSeeding(seed);
    return seed->count;
  }
  return 0;
}

void Processor::timerCallback() {
  drainRuntimeLogs();
  if (!retirements_.empty())
    worker_->requestRetirementCollection();
  const auto notifyProjectChange = worker_->takeProjectStateChange();
  const auto expected = expectedBlockSize_.load(std::memory_order_acquire);
  const auto needsResize =
      expected > 0 &&
      expected != configuredBlockSize_.load(std::memory_order_acquire);
  const auto needsRecovery =
      runtimeRecoveryRequested_.load(std::memory_order_acquire);
  if (!worker_->hasPendingSeedValues() && !needsResize && !needsRecovery) {
    if (notifyProjectChange)
      updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                            .withNonParameterStateChanged(true));
    return;
  }

  std::unique_lock preparationLock(preparationMutex_, std::try_to_lock);
  if (!preparationLock.owns_lock()) {
    if (notifyProjectChange)
      updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                            .withNonParameterStateChanged(true));
    return;
  }
  const auto seeded = applySeedValues();
  if (needsResize) {
    for (auto &channel : dryScratch_)
      channel.resize(static_cast<std::size_t>(expected));
    worker_->configure(expectedSampleRate_.load(std::memory_order_acquire),
                       expected);
    configuredBlockSize_.store(expected, std::memory_order_release);
  }
  if (runtimeRecoveryRequested_.exchange(false, std::memory_order_acq_rel))
    worker_->requestRebuild();
  preparationLock.unlock();
  notifySlotValues(seeded);
  if (notifyProjectChange)
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                          .withNonParameterStateChanged(true));
}

void Processor::handleAsyncUpdate() {
  if (refreshSlotParameterInfo())
    updateHostDisplay(
        juce::AudioProcessorListener::ChangeDetails{}.withParameterInfoChanged(
            true));
}

bool Processor::refreshSlotParameterInfo() {
  const auto mappings = worker_->parameterMappings();
  auto changed = false;
  for (std::size_t index = 0; index < slotCount; ++index) {
    const auto *mapping = index < mappings.size() ? &mappings[index] : nullptr;
    changed = presentedSlotParameters_[index]->setMapping(mapping) || changed;
  }
  return changed;
}

void Processor::getStateInformation(juce::MemoryBlock &destination) {
  juce::MemoryOutputStream stream(destination, false);
  stream.writeInt(static_cast<int>(stateMagic));
  stream.writeInt(stateVersion);
  const auto snapshot = [this] {
    std::lock_guard preparationLock(preparationMutex_);
    return worker_->persistedStateSnapshot();
  }();
  const auto &project = snapshot.project;
  stream.writeString(pathToJuce(project.path));
  {
    std::lock_guard lock(stateMutex_);
    stream.writeString(pathToJuce(lastBrowseDirectory_));
  }
  stream.writeInt(static_cast<int>(project.bufferBindings.size()));
  for (const auto &binding : project.bufferBindings) {
    stream.writeString(juce::String::fromUTF8(binding.name.c_str()));
    stream.writeString(pathToJuce(binding.path));
  }
  writeProjectImage(stream, project.projectImage);
  for (std::size_t index = 0; index < slotCount; ++index)
    stream.writeFloat(normalized(snapshot.parameters[index]));
  stream.writeInt(editorWidth_.load(std::memory_order_relaxed));
  stream.writeInt(editorHeight_.load(std::memory_order_relaxed));
  stream.writeBool(paramControlLayout() == ParamControlLayout::knobs);
}

void Processor::setStateInformation(const void *data, const int byteCount) {
  if (data == nullptr || byteCount <= 0)
    return;
  try {
    juce::MemoryInputStream stream(data, static_cast<std::size_t>(byteCount),
                                   false);
    if (stream.readInt() != static_cast<int>(stateMagic) ||
        stream.readInt() != stateVersion) {
      return;
    }
    const auto path = readBoundedString(stream, maximumPathBytes);
    const auto browseDirectory = readBoundedString(stream, maximumPathBytes);
    if (!path || !browseDirectory)
      return;

    const auto bufferCount = stream.readInt();
    if (bufferCount < 0 || bufferCount > maximumBufferBindings)
      return;
    std::vector<BufferFileBinding> bufferBindings;
    bufferBindings.reserve(static_cast<std::size_t>(bufferCount));
    std::unordered_set<std::string> bufferNames;
    for (int index = 0; index < bufferCount; ++index) {
      const auto name = readBoundedString(stream, maximumBufferNameBytes);
      const auto bufferPath = readBoundedString(stream, maximumPathBytes);
      if (!name || !bufferPath || name->isEmpty() || bufferPath->isEmpty() ||
          !bufferNames.insert(name->toStdString()).second) {
        return;
      }
      bufferBindings.push_back(
          {.name = name->toStdString(), .path = pathFromJuce(*bufferPath)});
    }

    auto validProjectImage = true;
    auto projectImage = readProjectImage(stream, validProjectImage);
    if (!validProjectImage)
      return;

    std::array<float, slotCount> restored{};
    for (auto &value : restored) {
      if (stream.getNumBytesRemaining() <
          static_cast<juce::int64>(sizeof(float)))
        return;
      value = normalized(stream.readFloat());
    }
    if (stream.getNumBytesRemaining() < 9U)
      return;
    const auto width = stream.readInt();
    const auto height = stream.readInt();
    const auto layout = stream.readBool() ? ParamControlLayout::knobs
                                          : ParamControlLayout::sliders;

    std::unique_lock preparationLock(preparationMutex_);
    deactivateRequested_.store(true, std::memory_order_release);
    for (std::size_t index = 0; index < slotCount; ++index)
      commitSlotValue(index, restored[index]);
    if (path->isEmpty() && !projectImage.valid()) {
      unload(false);
    } else {
      worker_->restore(pathFromJuce(*path), std::move(projectImage),
                       std::move(bufferBindings));
    }
    editorWidth_.store(std::clamp(width, 360, 1600), std::memory_order_relaxed);
    editorHeight_.store(std::clamp(height, 480, 1400),
                        std::memory_order_relaxed);
    paramControlLayout_.store(layout, std::memory_order_relaxed);
    {
      std::lock_guard lock(stateMutex_);
      lastBrowseDirectory_ = pathFromJuce(*browseDirectory);
    }
    preparationLock.unlock();
    notifySlotValues(slotCount);
  } catch (...) {
    // Host-provided state is untrusted and must never escape the VST callback.
  }
}

} // namespace onda::plugin
