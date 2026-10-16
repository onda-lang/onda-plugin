#include "Processor.h"

#include "Editor.h"
#include "HostParameter.h"
#include "JucePath.h"
#include "ProjectExport.h"
#include "ProjectPath.h"
#include "StateFormat.h"

#include <onda_processor_abi.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <tuple>
#include <unordered_set>

namespace onda::plugin {
namespace {

constexpr int maximumPathBytes = 16 * 1024;
constexpr int maximumBufferNameBytes = 1024;
constexpr int maximumBufferBindings = 1024;
constexpr int maximumViewStateBytes = 256 * 1024;
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

std::string controlId(const std::size_t index) {
  return "input" + (index < 9U ? std::string{"0"} : std::string{}) +
         std::to_string(index + 1U);
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

Processor::Processor(const Product product)
    : AudioProcessor(buses()), product_(product) {
  for (std::size_t index = 0; index < slotCount; ++index) {
    const auto id = slotId(index);
    hostParameterIds_[index] = {
        juce::VST3ClientExtensions::convertJuceParameterId(id), index, false};
    auto parameter = std::make_unique<HostParameter>(
        id, "Slot " + juce::String(static_cast<int>(index + 1U)),
        slotState_[index], true);
    slotParameters_[index] = parameter.get();
    addParameter(parameter.release());
  }
  for (std::size_t index = 0; index < controlCount; ++index) {
    const auto id = controlId(index);
    hostParameterIds_[slotCount + index] = {
        juce::VST3ClientExtensions::convertJuceParameterId(id), index, true};
    auto parameter = std::make_unique<HostParameter>(
        id, "Control " + juce::String(static_cast<int>(index + 1U)),
        controlState_[index], false);
    controlParameters_[index] = parameter.get();
    addParameter(parameter.release());
  }
  std::ranges::sort(hostParameterIds_, {}, &HostParameterId::id);
  worker_ = std::make_unique<Worker>(
      product_, replacements_, retirements_, deactivateRequested_,
      Worker::BuildFunction{}, nullptr,
      [this] {
        HostParameterValues values;
        for (std::size_t index = 0; index < slotCount; ++index) {
          const auto target = slotState_.preciseSnapshot(index);
          values.parameters[index] = target.value;
          values.parameterStamps[index] = target.stamp;
        }
        for (std::size_t index = 0; index < controlCount; ++index) {
          const auto target = controlState_.preciseSnapshot(index);
          values.controls[index] = target.value;
          values.controlStamps[index] = target.stamp;
        }
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
  delete scratchReplacements_.tryPop();
  delete scratchRetirements_.tryPop();
  delete dryScratch_;
}

juce::AudioProcessor::BusesProperties Processor::buses() {
  BusesProperties result;
  if constexpr (pluginInputChannels > 0) {
    result = result.withInput(
        "Input",
        juce::AudioChannelSet::canonicalChannelSet(pluginInputChannels), true);
  }
  for (int index = 0; index < pluginControlInputs; ++index)
    result = result.withInput("Control " + juce::String(index + 1),
                              juce::AudioChannelSet::mono(), false);
  return result.withOutput(
      "Output",
      juce::AudioChannelSet::canonicalChannelSet(pluginOutputChannels), true);
}

void Processor::prepareToPlay(const double sampleRate,
                              const int maximumBlockSize) {
  std::unique_lock preparationLock(preparationMutex_);
  const auto safeBlockSize = std::max(maximumBlockSize, 1);
  const auto oldOndaBlockSize = ondaBlockSize();
  const auto preference = preferredBlockSize();
  const auto ondaSize =
      preference > 0 ? std::min(preference, safeBlockSize) : safeBlockSize;
  const auto changed =
      !sameSampleRate(sampleRate,
                      expectedSampleRate_.load(std::memory_order_acquire)) ||
      ondaSize != oldOndaBlockSize;
  expectedSampleRate_.store(sampleRate, std::memory_order_release);
  expectedBlockSize_.store(safeBlockSize, std::memory_order_release);
  auto scratch = std::make_unique<DryScratch>(safeBlockSize);
  // Host preparation runs with audio suspended; reclaim all scratch directly.
  delete scratchReplacements_.tryPop();
  delete scratchRetirements_.tryPop();
  delete std::exchange(dryScratch_, scratch.release());
  bypassMix_.reset(sampleRate, 0.005);
  bypassStarted_ = false;
  if (changed) {
    deactivateRequested_.store(true, std::memory_order_release);
  }
  clearMidiActivity();
  worker_->configure(sampleRate, ondaSize, fastMath());
  configuredBlockSize_.store(safeBlockSize, std::memory_order_release);
  synchronizeEngine();
  preparationLock.unlock();
  notifyDeferredAutomation();
}

void Processor::synchronizeEngine() {
  // Called with audio suspended during host preparation, or from an offline
  // callback. Neither path needs a message-loop tick to finish preparation.
  offlinePreparedGeneration_ = worker_->waitForPreparation();
  delete retirements_.tryPop();
  acquireEngine();
  applySeedValues();
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
  if (layouts.inputBuses.size() != pluginInputBuses ||
      layouts.outputBuses.size() != 1)
    return false;
  const auto expectedOutput =
      juce::AudioChannelSet::canonicalChannelSet(pluginOutputChannels);
  if (layouts.getMainOutputChannelSet() != expectedOutput)
    return false;
  if constexpr (pluginInputChannels > 0) {
    if (layouts.inputBuses[0] !=
        juce::AudioChannelSet::canonicalChannelSet(pluginInputChannels))
      return false;
  }
  for (int index = pluginMainInputBuses; index < pluginInputBuses; ++index) {
    const auto &layout = layouts.inputBuses[index];
    if (!layout.isDisabled() && layout != juce::AudioChannelSet::mono())
      return false;
  }
  return true;
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
  if (auto *seed = active_->parameterSeed();
      seed != nullptr && !seed->started.load(std::memory_order_acquire)) {
    for (std::size_t index = 0; index < seed->count; ++index)
      seed->parameterStamps.before[index] = slotState_.snapshot(index).stamp;
    for (std::size_t index = 0; index < seed->controlCount; ++index)
      seed->controlStamps.before[index] = controlState_.snapshot(index).stamp;
    seed->started.store(true, std::memory_order_release);
  }
  worker_->setActiveGeneration(replacement->buildGeneration());
  const auto initialized = active_->initializeControlDefaults(
      controlState_, controlAutomation_.retainedLanes());
  detail::forEachAutomationLane(
      initialized,
      [this](const std::size_t index) noexcept {
        controlAutomation_.notify(index);
      });
  if (initialized != 0U)
    defaultValuesChanged_.store(true, std::memory_order_release);
  clearMidiActivity();
  scopeCapture_.requestReset();
  runtimeFaulted_.store(false, std::memory_order_release);
}

void Processor::acquireDryScratch() noexcept {
  if (scratchReplacements_.empty() || !scratchRetirements_.empty())
    return;
  if (auto *replacement = scratchReplacements_.tryPop()) {
    if (dryScratch_ != nullptr) {
      const auto retired = scratchRetirements_.tryPush(dryScratch_);
      jassert(retired);
      juce::ignoreUnused(retired);
    }
    dryScratch_ = replacement;
  }
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

void Processor::restoreDry(float *const *outputs, const int offset,
                           const int frames) noexcept {
  for (int channel = 0; channel < pluginOutputChannels; ++channel) {
    auto *output = outputs[channel] + offset;
    if (product_ == Product::effect && channel < pluginPassthroughChannels)
      std::copy_n(dryScratch_->channel(static_cast<std::size_t>(channel)) + offset,
                  frames, output);
    else
      std::fill_n(output, frames, 0.0F);
  }
}

void Processor::applyBypass(float *const *outputs, const int frames,
                            const bool bypassed) noexcept {
  if (frames <= 0)
    return;
  const auto target = bypassed ? 0.0 : 1.0;
  if (!bypassStarted_) {
    bypassMix_.setCurrentAndTargetValue(target);
    bypassStarted_ = true;
  } else {
    bypassMix_.setTargetValue(target);
  }
  // Advance once per real sample, after all DSP/MIDI segments. Retargeting
  // starts at the current mix, and settled wet output needs no extra pass.
  int frame = 0;
  while (frame < frames && bypassMix_.isSmoothing()) {
    const auto wet = bypassMix_.getNextValue();
    for (int channel = 0; channel < pluginOutputChannels; ++channel) {
      const auto dry =
          product_ == Product::effect && channel < pluginPassthroughChannels
              ? dryScratch_->channel(static_cast<std::size_t>(channel))[frame]
              : 0.0F;
      outputs[channel][frame] =
          wet <= 0.0
              ? dry
              : static_cast<float>(std::lerp(
                    static_cast<double>(dry),
                    static_cast<double>(outputs[channel][frame]), wet));
    }
    ++frame;
  }
  if (bypassed)
    restoreDry(outputs, frame, frames - frame);
}

void Processor::processBlock(juce::AudioBuffer<float> &audio,
                             juce::MidiBuffer &midi) {
  processAudio(audio, midi, false);
}

void Processor::processBlockBypassed(juce::AudioBuffer<float> &audio,
                                     juce::MidiBuffer &midi) {
  processAudio(audio, midi, true);
}

void Processor::processAudio(juce::AudioBuffer<float> &audio,
                             juce::MidiBuffer &midi, const bool bypassed) {
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
      synchronizeEngine();
      preparationLock.unlock();
      notifyDeferredAutomation();
    }
  }
  acquireDryScratch();
  const auto maximum = expectedBlockSize_.load(std::memory_order_acquire);
  if (frames < 0 || frames > maximum || maximum <= 0 ||
      dryScratch_ == nullptr || frames > dryScratch_->capacity) {
    fallback(audio);
    if (frames > maximum && frames > 0) {
      if (preferredBlockSize() == 0 || preferredBlockSize() > maximum)
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
      active_->blockSize() != ondaBlockSize()) {
    fallback(audio);
    return;
  }

  active_->beginHostCallback(
      frames, &hostAutomation_.curves, &controlState_,
      &controlAutomation_.curves,
      automationCaptured_ && automationCapturedDefaults_ &&
          automationCapturedSeed_ == active_->parameterSeedRevision());
  reconcileHostAutomation(active_,
                          active_->hostCallbackUsesParameterDefaults());

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
      !active_->reset(slotState_)) {
    faultActive();
    fallback(audio);
    return;
  }

  if (parameterSettleRequested_.exchange(false, std::memory_order_acq_rel))
    active_->settleParameters();
  if (!active_->setParameterSmoothingSeconds(paramSmoothingSeconds()) ||
      !active_->setInputControlSmoothingSeconds(
          inputControlSmoothingSeconds())) {
    faultActive();
    fallback(audio);
    return;
  }
  for (std::size_t count = 0;
       count < userEventQueueCapacity && userEvents_.tryPop(userEventScratch_);
       ++count) {
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
        slotState_, hostContext);
    if (result == EventTriggerResult::inputRejected)
      continue;
    if (result == EventTriggerResult::runtimeFailure) {
      faultActive();
      fallback(audio);
      return;
    }
  }

  if (product_ == Product::effect) {
    for (int channel = 0; channel < pluginPassthroughChannels; ++channel) {
      std::copy_n(audio.getReadPointer(channel), frames,
                  dryScratch_->channel(static_cast<std::size_t>(channel)));
    }
  }

  const auto inputCount = active_->inputChannels();
  // Only the program's declared input prefix is initialized and consumed.
  std::array<float *, static_cast<std::size_t>(pluginTotalInputChannels)>
      inputs;
  for (int channel = 0; channel < std::min(inputCount, pluginInputChannels);
       ++channel)
    inputs[static_cast<std::size_t>(channel)] = audio.getWritePointer(channel);
  // JUCE packs enabled buses, while Onda's control channel positions are fixed.
  // Null pointers leave disconnected controls at their prepared defaults.
  auto hostChannel = pluginInputChannels;
  for (int control = 0; control < inputCount - pluginInputChannels; ++control) {
    auto &input =
        inputs[static_cast<std::size_t>(pluginInputChannels + control)];
    if (getChannelCountOfBus(true, pluginMainInputBuses + control) != 0) {
      input = audio.getWritePointer(hostChannel++);
    } else {
      input = nullptr;
    }
  }
  std::array<float *, static_cast<std::size_t>(pluginOutputChannels)> outputs{};
  for (int channel = 0; channel < pluginOutputChannels; ++channel)
    outputs[static_cast<std::size_t>(channel)] = audio.getWritePointer(channel);
  const auto processRange =
      [this, &inputs, &outputs, inputCount,
       &hostContext](const int offset, const int count,
                     const std::span<const MidiEvent> events) noexcept {
        std::array<float *, static_cast<std::size_t>(pluginTotalInputChannels)>
            shiftedInputs;
        for (int channel = 0; channel < inputCount; ++channel) {
          auto *input = inputs[static_cast<std::size_t>(channel)];
          shiftedInputs[static_cast<std::size_t>(channel)] =
              input != nullptr ? input + offset : nullptr;
        }
        std::array<float *, static_cast<std::size_t>(pluginOutputChannels)>
            shiftedOutputs{};
        for (int channel = 0; channel < pluginOutputChannels; ++channel) {
          shiftedOutputs[static_cast<std::size_t>(channel)] =
              outputs[static_cast<std::size_t>(channel)] + offset;
        }
        return active_->process(shiftedInputs.data(), shiftedOutputs.data(),
                                count, events, slotState_, hostContext, offset);
      };

  if (keyboardGeneration_ != active_->buildGeneration()) {
    keyboardHeldNotes_.fill(false);
    keyboardGeneration_ = active_->buildGeneration();
  }
  const auto dispatchKeyboardNote = [&](const int key, const float velocity) {
    auto &held = keyboardHeldNotes_[static_cast<std::size_t>(key)];
    // Adoption and queue cancellation invalidate old virtual presses. Their
    // later releases must not stop a host note on the active engine.
    if (velocity <= 0.0F && !held)
      return true;
    const MidiEvent event{velocity > 0.0F ? MidiKind::noteOn
                                          : MidiKind::noteOff,
                          0, 0, key, velocity};
    if (!processRange(0, 0, std::span<const MidiEvent>{&event, 1U}))
      return false;
    held = velocity > 0.0F;
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
    restoreDry(outputs.data(), 0, frames);
  } else {
    applyBypass(outputs.data(), frames, bypassed);
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

void Processor::processBlockBypassed(juce::AudioBuffer<double> &audio,
                                     juce::MidiBuffer &midi) {
  processBlock(audio, midi);
}

juce::AudioProcessorEditor *Processor::createEditor() {
  return new Editor(*this);
}

void Processor::loadFile(const juce::File &file, const bool seedDefaults) {
  if (file.existsAsFile()) {
    std::lock_guard preparationLock(preparationMutex_);
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
    exportPool_.addJob([this, snapshot, directory,
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

void Processor::clearRuntimeState() {
  runtimeFaulted_.store(false, std::memory_order_release);
  runtimeRecoveryRequested_.store(false, std::memory_order_release);
  resetRequested_.store(false, std::memory_order_release);
  parameterSettleRequested_.store(false, std::memory_order_release);
  activeLogGeneration_.store(0U, std::memory_order_release);
  clearMidiActivity();
  clearRuntimeLog();
}

void Processor::unload(const bool notifyHost) {
  clearRuntimeState();
  worker_->unload(notifyHost);
}

void Processor::requestReload() { worker_->requestRebuild(); }

void Processor::resetParametersToDefaults(const ParameterReset bank) {
  const auto controls =
      worker_->resetParameters(slotState_, controlState_, bank);
  if (bank != ParameterReset::controls)
    parameterSettleRequested_.store(true, std::memory_order_release);
  for (std::size_t index = 0;
       bank != ParameterReset::controls && index < slotCount; ++index) {
    beginSlotGesture(index);
    slotParameters_[index]->notifyEditorValue();
    endSlotGesture(index);
  }
  for (std::size_t index = 0;
       bank != ParameterReset::parameters && index < controls; ++index) {
    auto *parameter = controlParameters_[index];
    parameter->beginChangeGesture();
    parameter->notifyEditorValue();
    parameter->endChangeGesture();
  }
}

void Processor::requestUserReset() {
  resetParametersToDefaults();
  reset();
}

std::string Processor::triggerEvent(const std::uint64_t generation,
                                    const std::string_view name,
                                    const juce::var &values) {
  const auto status = worker_->status();
  if (!status.active || status.engineGeneration == 0)
    return "No active Onda program";
  if (generation != status.engineGeneration)
    return "The Onda program changed; retry the event after the editor "
           "refreshes";
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
  command.generation = generation;
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
    status.hasError = true;
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
  return index < slotCount ? static_cast<float>(slotState_.value(index)) : 0.5F;
}

std::uint64_t Processor::slotStamp(const std::size_t index) const noexcept {
  return index < slotCount ? slotState_.snapshot(index).stamp : 0U;
}

HostParameterValues Processor::hostParameterValues() const {
  return worker_->hostParameterValues();
}

void Processor::beginSlotGesture(const std::size_t index) {
  if (index < slotCount)
    slotParameters_[index]->beginChangeGesture();
}

void Processor::setSlotValue(const std::size_t index, const float value) {
  if (index < slotCount)
    slotParameters_[index]->setEditorValue(normalized(value));
}

void Processor::endSlotGesture(const std::size_t index) {
  if (index < slotCount)
    slotParameters_[index]->endChangeGesture();
}

float Processor::inputControlValue(const std::size_t index) const noexcept {
  return index < controlCount ? static_cast<float>(controlState_.value(index))
                              : 0.5F;
}

std::uint64_t
Processor::inputControlStamp(const std::size_t index) const noexcept {
  return index < controlCount ? controlState_.snapshot(index).stamp : 0U;
}

void Processor::beginInputControlGesture(const std::size_t index) {
  if (index < controlCount)
    controlParameters_[index]->beginChangeGesture();
}

void Processor::setInputControlValue(const std::size_t index,
                                     const float value) {
  if (index < controlCount)
    controlParameters_[index]->setEditorValue(normalized(value));
}

void Processor::endInputControlGesture(const std::size_t index) {
  if (index < controlCount)
    controlParameters_[index]->endChangeGesture();
}

juce::String Processor::controlInputName(const std::size_t index) const {
  return index < controlCount ? controlParameters_[index]->getName(128)
                              : juce::String{};
}

void Processor::clearHostAutomation() noexcept {
  hostAutomation_.curves.clear();
  controlAutomation_.curves.clear();
  automationCaptured_ = false;
  automationPublication_.reset();
}

const PreparedEngine *Processor::automationEngine() const noexcept {
  // Faults and deactivation can precede retirement during parameter-only flushes.
  if (activeFaulted_ || deactivateRequested_.load(std::memory_order_acquire))
    return nullptr;
  return active_;
}

void Processor::publishHostAutomationTargets() noexcept {
  const auto *engine = automationEngine();
  const auto useDefaults =
      engine != nullptr &&
      (engine->parameterDefaultsPending() ||
       (automationCaptured_ && automationCapturedDefaults_ &&
        automationCapturedSeed_ == engine->parameterSeedRevision()));
  reconcileHostAutomation(engine, useDefaults);
}

void Processor::reconcileHostAutomation(const PreparedEngine *const engine,
                                        const bool useDefaults) noexcept {
  const auto *seed = engine != nullptr ? engine->parameterSeed() : nullptr;
  const auto acknowledged =
      seed != nullptr && seed->applied.load(std::memory_order_acquire);
  const AutomationPublication publication{seed != nullptr ? seed->revision : 0U,
                                          useDefaults, acknowledged};
  // Capture invalidates this cache. Revisit published endpoints only when
  // adoption or seed acknowledgment changes their publication context.
  if (automationPublication_ == publication)
    return;
  hostAutomation_.publish(publication.seed, useDefaults,
                          acknowledged ? &seed->parameterStamps : nullptr);
  controlAutomation_.publish(publication.seed, useDefaults,
                             acknowledged ? &seed->controlStamps : nullptr);
  automationPublication_ = publication;
}

bool Processor::ownsHostParameter(const std::uint32_t id) const noexcept {
  const auto found =
      std::ranges::lower_bound(hostParameterIds_, id, {}, &HostParameterId::id);
  return found != hostParameterIds_.end() && found->id == id;
}

void Processor::captureHostAutomation(const std::uint32_t id,
                                      const AutomationQueue queue,
                                      const int frames) noexcept {
  const auto *engine = automationEngine();
  const auto *seed = engine != nullptr ? engine->parameterSeed() : nullptr;
  const auto acknowledged =
      seed != nullptr && seed->applied.load(std::memory_order_acquire);
  const auto found =
      std::ranges::lower_bound(hostParameterIds_, id, {}, &HostParameterId::id);
  if (found == hostParameterIds_.end() || found->id != id)
    return;
  automationPublication_.reset();
  if (!automationCaptured_) {
    automationCaptured_ = true;
    automationCapturedDefaults_ = seed != nullptr && !acknowledged;
    automationCapturedSeed_ = seed != nullptr ? seed->revision : 0U;
  }
  if (found->control) {
    const auto target = engine != nullptr
                            ? engine->controlTarget(found->index, controlState_)
                            : controlState_.target(found->index);
    controlAutomation_.capture(
        found->index, queue, frames, seed != nullptr ? seed->revision : 0U,
        acknowledged ? &seed->controlStamps : nullptr,
        engine != nullptr &&
            engine->inputControlRampPending(found->index, target.value,
                                            inputControlSmoothingSeconds()),
        target);
  } else {
    const auto target = engine != nullptr
                            ? engine->parameterTarget(found->index, slotState_)
                            : slotState_.target(found->index);
    hostAutomation_.capture(
        found->index, queue, frames, seed != nullptr ? seed->revision : 0U,
        acknowledged ? &seed->parameterStamps : nullptr,
        engine != nullptr &&
            !parameterSettleRequested_.load(std::memory_order_acquire) &&
            engine->editorRampPending(found->index, target.value,
                                      paramSmoothingSeconds()),
        target);
  }
}

bool Processor::setFastMath(const bool enabled) {
  std::unique_lock lock(preparationMutex_);
  if (!canSetBlockSize())
    return false;
  if (fastMath() == enabled)
    return true;
  fastMath_.store(enabled, std::memory_order_release);
  worker_->configure(hostSampleRate(), ondaBlockSize(), enabled);
  lock.unlock();
  updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                        .withNonParameterStateChanged(true));
  return true;
}

int Processor::ondaBlockSize() const noexcept {
  const auto maximum = hostBlockSize();
  const auto preference = preferredBlockSize();
  return preference > 0 ? std::min(preference, maximum) : maximum;
}

bool Processor::canSetBlockSize() const {
  const auto status = worker_->status();
  return status.path.empty() && !status.active && !status.compiling;
}

bool Processor::setPreferredBlockSize(const int samples) {
  std::unique_lock lock(preparationMutex_);
  if (samples < 0 || (samples > 0 && samples > hostBlockSize()) ||
      !canSetBlockSize())
    return false;
  if (preferredBlockSize() == samples)
    return true;
  preferredBlockSize_.store(samples, std::memory_order_release);
  if (hostBlockSize() > 0)
    worker_->configure(hostSampleRate(), ondaBlockSize(), fastMath());
  lock.unlock();
  updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                        .withNonParameterStateChanged(true));
  return true;
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

double Processor::paramSmoothingSeconds() const noexcept {
  return paramSmoothingSeconds_.load(std::memory_order_relaxed);
}

bool Processor::setParamSmoothingSeconds(const double seconds) {
  if (!parameterSmoothingSamples(seconds, std::max(1.0, hostSampleRate())))
    return false;
  if (!std::is_eq(paramSmoothingSeconds_.exchange(
                      seconds, std::memory_order_relaxed) <=> seconds))
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                          .withNonParameterStateChanged(true));
  return true;
}

ParamControlLayout Processor::inputControlLayout() const noexcept {
  return inputControlLayout_.load(std::memory_order_relaxed);
}

void Processor::setInputControlLayout(const ParamControlLayout layout) {
  if (inputControlLayout_.exchange(layout, std::memory_order_relaxed) != layout)
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                          .withNonParameterStateChanged(true));
}

double Processor::inputControlSmoothingSeconds() const noexcept {
  return inputControlSmoothingSeconds_.load(std::memory_order_relaxed);
}

bool Processor::setInputControlSmoothingSeconds(const double seconds) {
  if (!parameterSmoothingSamples(seconds, std::max(1.0, hostSampleRate())))
    return false;
  if (!std::is_eq(inputControlSmoothingSeconds_.exchange(
                      seconds, std::memory_order_relaxed) <=> seconds))
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                          .withNonParameterStateChanged(true));
  return true;
}

juce::String Processor::viewState() const {
  std::lock_guard lock(stateMutex_);
  return viewState_;
}

void Processor::setViewState(const juce::var &state) {
  if (state.getDynamicObject() == nullptr)
    return;
  const auto json = juce::JSON::toString(state, true);
  if (json.getNumBytesAsUTF8() > maximumViewStateBytes)
    return;
  {
    std::lock_guard lock(stateMutex_);
    if (viewState_ == json)
      return;
    viewState_ = json;
  }
  updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                        .withNonParameterStateChanged(true));
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

void Processor::notifyDeferredAutomation() {
  const auto *manager = juce::MessageManager::getInstanceWithoutCreating();
  if (manager == nullptr || !manager->isThisTheMessageThread())
    return;
  const auto notify = [](auto &automation, const auto &parameters) {
    detail::forEachAutomationLane(
        automation.takeNotifications(),
        [&](const std::size_t index) { parameters[index]->notifyHostValue(); });
  };
  notify(hostAutomation_, slotParameters_);
  notify(controlAutomation_, controlParameters_);
}

void Processor::applySeedValues() {
  if (auto seed = worker_->takeSeedValues()) {
    auto changed = false;
    // Conditional writes preserve edits since adoption. Publish every result
    // before acknowledgment lets audio reconcile retained queue endpoints.
    for (std::size_t index = 0; index < seed->count; ++index) {
      const auto stamp = slotState_.seed(
          index, static_cast<float>(seed->values[index].normalized),
          seed->parameterStamps.before[index],
          seed->parameterStamps.after[index],
          seed->values[index].scalarDefault.has_value());
      hostAutomation_.notify(index);
      changed = changed || stamp != 0U;
    }
    for (std::size_t index = 0; index < seed->controlCount; ++index) {
      const auto stamp = controlState_.seed(
          index, static_cast<float>(seed->controls[index].normalized),
          seed->controlStamps.before[index], seed->controlStamps.after[index],
          seed->controls[index].scalarDefault.has_value());
      controlAutomation_.notify(index);
      changed = changed || stamp != 0U;
    }
    worker_->finishSeeding(seed);
    if (changed)
      defaultValuesChanged_.store(true, std::memory_order_release);
  }
}

void Processor::timerCallback() {
  drainRuntimeLogs();
  // Snapshot before draining notifications so the controller is current when
  // the host refreshes its cache. Preparation can suppress wrapper restarts,
  // so deliver this only from the message-thread timer.
  const auto defaultsChanged =
      defaultValuesChanged_.exchange(false, std::memory_order_acq_rel);
  // Host/controller listeners may allocate or lock. Deferred DSP targets are
  // already committed; publish their current presentation on this thread.
  notifyDeferredAutomation();
  if (defaultsChanged)
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                          .withProgramChanged(true));
  if (!retirements_.empty())
    worker_->requestRetirementCollection();
  const auto notifyProjectChange = worker_->takeProjectStateChange();
  const auto expected = expectedBlockSize_.load(std::memory_order_acquire);
  const auto needsResize =
      expected > 0 &&
      expected != configuredBlockSize_.load(std::memory_order_acquire);
  const auto needsRecovery =
      runtimeRecoveryRequested_.load(std::memory_order_acquire);
  if (!worker_->hasPendingSeedValues() && !needsResize && !needsRecovery &&
      scratchRetirements_.empty()) {
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
  applySeedValues();
  delete scratchRetirements_.tryPop();
  if (needsResize) {
    auto scratch = std::make_unique<DryScratch>(expected);
    delete scratchReplacements_.replace(scratch.release());
    worker_->configure(expectedSampleRate_.load(std::memory_order_acquire),
                       ondaBlockSize(), fastMath());
    configuredBlockSize_.store(expected, std::memory_order_release);
  }
  if (runtimeRecoveryRequested_.exchange(false, std::memory_order_acq_rel))
    worker_->requestRebuild();
  preparationLock.unlock();
  notifyDeferredAutomation();
  if (notifyProjectChange)
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                          .withNonParameterStateChanged(true));
}

void Processor::handleAsyncUpdate() {
  if (refreshHostParameterInfo())
    updateHostDisplay(
        juce::AudioProcessorListener::ChangeDetails{}.withParameterInfoChanged(
            true));
}

bool Processor::refreshHostParameterInfo() {
  const auto status = worker_->status();
  const auto &mappings = status.mappings;
  auto changed = false;
  for (std::size_t index = 0; index < slotCount; ++index) {
    const auto *mapping = index < mappings.size() ? &mappings[index] : nullptr;
    changed = slotParameters_[index]->setMapping(mapping) || changed;
  }
  for (std::size_t index = 0; index < controlCount; ++index) {
    const auto *mapping =
        index < status.controls.size() ? &status.controls[index] : nullptr;
    changed = controlParameters_[index]->setMapping(mapping) || changed;
  }
  return changed;
}

void Processor::getStateInformation(juce::MemoryBlock &destination) {
  juce::MemoryOutputStream stream(destination, false);
  stream.writeInt(static_cast<int>(stateMagic));
  stream.writeInt(stateVersion);
  const auto [snapshot, blockPreference, fastMathEnabled] = [this] {
    std::lock_guard preparationLock(preparationMutex_);
    return std::tuple{worker_->persistedStateSnapshot(), preferredBlockSize(),
                      fastMath()};
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
  for (const auto &value : snapshot.values.parameters)
    writeScalarValue(stream, value);
  stream.writeInt(editorWidth_.load(std::memory_order_relaxed));
  stream.writeInt(editorHeight_.load(std::memory_order_relaxed));
  stream.writeBool(paramControlLayout() == ParamControlLayout::knobs);
  stream.writeString(viewState());
  stream.writeDouble(paramSmoothingSeconds());
  stream.writeInt(blockPreference);
  stream.writeBool(fastMathEnabled);
  stream.writeBool(inputControlLayout() == ParamControlLayout::knobs);
  stream.writeDouble(inputControlSmoothingSeconds());
  for (const auto &value : snapshot.values.controls)
    writeScalarValue(stream, value);
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
      if (!name || !bufferPath || name->isEmpty() ||
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

    HostParameterValues restored;
    for (auto &value : restored.parameters) {
      const auto parameter = readScalarValue(stream);
      if (!parameter)
        return;
      value = *parameter;
    }
    if (stream.getNumBytesRemaining() < 9U)
      return;
    const auto width = stream.readInt();
    const auto height = stream.readInt();
    const auto layout = stream.readBool() ? ParamControlLayout::knobs
                                          : ParamControlLayout::sliders;
    const auto viewState = readBoundedString(stream, maximumViewStateBytes);
    if (!viewState ||
        (viewState->isNotEmpty() &&
         juce::JSON::parse(*viewState).getDynamicObject() == nullptr))
      return;
    if (stream.getNumBytesRemaining() <
        static_cast<juce::int64>(sizeof(double)))
      return;
    const auto smoothingSeconds = stream.readDouble();
    if (!parameterSmoothingSamples(smoothingSeconds,
                                   std::max(1.0, hostSampleRate())))
      return;
    if (stream.getNumBytesRemaining() < 5)
      return;
    const auto blockPreference = stream.readInt();
    if (blockPreference < 0)
      return;
    const auto fastMathEnabled = stream.readBool();

    if (stream.getNumBytesRemaining() < 9)
      return;
    const auto inputLayout = stream.readBool() ? ParamControlLayout::knobs
                                               : ParamControlLayout::sliders;
    const auto inputSmoothing = stream.readDouble();
    if (!parameterSmoothingSamples(inputSmoothing,
                                   std::max(1.0, hostSampleRate())))
      return;

    for (auto &value : restored.controls) {
      const auto control = readScalarValue(stream);
      if (!control)
        return;
      value = *control;
    }

    std::unique_lock preparationLock(preparationMutex_);
    deactivateRequested_.store(true, std::memory_order_release);
    preferredBlockSize_.store(blockPreference, std::memory_order_release);
    fastMath_.store(fastMathEnabled, std::memory_order_release);
    if (hostBlockSize() > 0)
      worker_->configure(hostSampleRate(), ondaBlockSize(), fastMath());
    for (std::size_t index = 0; index < slotCount; ++index) {
      restored.parameterStamps[index] = slotState_[index].set(
          static_cast<float>(restored.parameters[index].normalized));
      hostAutomation_.notify(index);
    }
    for (std::size_t index = 0; index < controlCount; ++index) {
      restored.controlStamps[index] = controlState_[index].set(
          static_cast<float>(restored.controls[index].normalized));
      controlAutomation_.notify(index);
    }
    if (path->isEmpty() && !projectImage.valid())
      clearRuntimeState();
    worker_->restore(pathFromJuce(*path), std::move(projectImage),
                     std::move(bufferBindings),
                     Worker::ExistingEnginePolicy::deactivateImmediately,
                     std::move(restored));
    editorWidth_.store(std::clamp(width, 360, 1600), std::memory_order_relaxed);
    editorHeight_.store(std::clamp(height, 480, 1400),
                        std::memory_order_relaxed);
    paramControlLayout_.store(layout, std::memory_order_relaxed);
    inputControlLayout_.store(inputLayout, std::memory_order_relaxed);
    inputControlSmoothingSeconds_.store(inputSmoothing,
                                        std::memory_order_relaxed);
    paramSmoothingSeconds_.store(smoothingSeconds, std::memory_order_relaxed);
    {
      std::lock_guard lock(stateMutex_);
      lastBrowseDirectory_ = pathFromJuce(*browseDirectory);
      viewState_ = *viewState;
    }
    viewStateRestoreRevision_.fetch_add(1U, std::memory_order_release);
    preparationLock.unlock();
    notifyDeferredAutomation();
  } catch (...) {
    // Host-provided state is untrusted and must never escape the VST callback.
  }
}

} // namespace onda::plugin
