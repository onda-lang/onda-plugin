#include "Processor.h"

#include "Editor.h"
#include "JucePath.h"
#include "ProjectExport.h"
#include "ProjectPath.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <system_error>
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

template <typename Value>
bool appendEventValue(UserEventCommand &command, const Value value) noexcept {
  if (command.payloadBytes > command.payload.size() - sizeof(value))
    return false;
  std::memcpy(command.payload.data() + command.payloadBytes, &value,
              sizeof(value));
  command.payloadBytes += static_cast<std::uint32_t>(sizeof(value));
  return true;
}

std::optional<double> eventNumber(const juce::var &value) {
  if (!value.isInt() && !value.isInt64() && !value.isDouble())
    return std::nullopt;
  const auto number = static_cast<double>(value);
  return std::isfinite(number) ? std::optional<double>{number} : std::nullopt;
}

bool isInteger(const double value) noexcept {
  return std::fpclassify(std::remainder(value, 1.0)) == FP_ZERO;
}

bool appendEventScalar(UserEventCommand &command, const int type,
                       const juce::var &value) {
  if (type == ONDA_PRIMITIVE_BOOL)
    return value.isBool() &&
           appendEventValue(
               command, static_cast<std::uint8_t>(static_cast<bool>(value)));

  if (type == ONDA_PRIMITIVE_I64) {
    if (value.isString()) {
      const auto text = value.toString().toStdString();
      std::int64_t integer{};
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), integer);
      return parsed.ec == std::errc{} &&
             parsed.ptr == text.data() + text.size() &&
             appendEventValue(command, integer);
    }
    if (value.isInt())
      return appendEventValue(
          command, static_cast<std::int64_t>(static_cast<int>(value)));
    if (value.isInt64())
      return appendEventValue(
          command, static_cast<std::int64_t>(static_cast<juce::int64>(value)));
  }

  const auto number = eventNumber(value);
  if (!number)
    return false;
  switch (type) {
  case ONDA_PRIMITIVE_F32:
    return *number >= -static_cast<double>(std::numeric_limits<float>::max()) &&
           *number <= static_cast<double>(std::numeric_limits<float>::max()) &&
           appendEventValue(command, static_cast<float>(*number));
  case ONDA_PRIMITIVE_F64:
    return appendEventValue(command, *number);
  case ONDA_PRIMITIVE_I32:
    return isInteger(*number) &&
           *number >=
               static_cast<double>(std::numeric_limits<std::int32_t>::min()) &&
           *number <=
               static_cast<double>(std::numeric_limits<std::int32_t>::max()) &&
           appendEventValue(command, static_cast<std::int32_t>(*number));
  case ONDA_PRIMITIVE_I64: {
    // A double cannot identify integers outside JavaScript's exact range.
    // Full-range i64 values cross the UI boundary as decimal strings.
    constexpr auto maximumExactDoubleInteger = 9'007'199'254'740'991.0;
    return isInteger(*number) && *number >= -maximumExactDoubleInteger &&
           *number <= maximumExactDoubleInteger &&
           appendEventValue(command, static_cast<std::int64_t>(*number));
  }
  default:
    return false;
  }
}

bool appendEventParameter(UserEventCommand &command,
                          const EventParameterMapping &parameter,
                          const juce::var &value) {
  if (!parameter.array && !parameter.slice)
    return appendEventScalar(command, parameter.elementType, value);
  const auto *values = value.getArray();
  if (values == nullptr ||
      (parameter.array && values->size() != parameter.arrayLength) ||
      values->size() > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  if (parameter.slice &&
      !appendEventValue(command, static_cast<std::int32_t>(values->size()))) {
    return false;
  }
  return std::ranges::all_of(*values, [&command, &parameter](const auto &item) {
    return appendEventScalar(command, parameter.elementType, item);
  });
}

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
    : AudioProcessor(buses()), product_(product),
      parameterState_(*this, nullptr, "OndaSlots", parameters()) {
  for (std::size_t index = 0; index < slotCount; ++index) {
    const auto id = slotId(index);
    slotAtomics_[index] = parameterState_.getRawParameterValue(id);
    slotParameters_[index] = parameterState_.getParameter(id);
    jassert(slotAtomics_[index] != nullptr &&
            slotParameters_[index] != nullptr);
  }
  worker_ =
      std::make_unique<Worker>(product_, replacements_, retirements_,
                               deactivateRequested_, replacementSeedPending_);
  startTimerHz(20);
}

Processor::~Processor() {
  stopTimer();
  exportPool_.removeAllJobs(false, -1);
  worker_.reset();
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
  for (std::size_t index = 0; index < slotCount; ++index) {
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{slotId(index), 1},
        "Slot " + juce::String(static_cast<int>(index + 1U)),
        juce::NormalisableRange<float>{0.0F, 1.0F}, 0.5F));
  }
  return layout;
}

void Processor::prepareToPlay(const double sampleRate,
                              const int maximumBlockSize) {
  const auto safeBlockSize = std::max(maximumBlockSize, 1);
  expectedSampleRate_.store(sampleRate, std::memory_order_release);
  expectedBlockSize_.store(safeBlockSize, std::memory_order_release);
  for (auto &channel : dryScratch_)
    channel.resize(static_cast<std::size_t>(safeBlockSize));
  deactivateRequested_.store(true, std::memory_order_release);
  replacementSeedPending_.store(false, std::memory_order_release);
  clearMidiActivity();
  worker_->configure(sampleRate, safeBlockSize);
  configuredBlockSize_.store(safeBlockSize, std::memory_order_release);
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
    clearMidiActivity();
  }
}

void Processor::acquireEngine() noexcept {
  if (deactivateRequested_.load(std::memory_order_acquire)) {
    retireActive();
    if (active_ == nullptr && retirements_.empty() &&
        !replacementSeedPending_.load(std::memory_order_acquire) &&
        !replacements_.empty()) {
      if (auto *stale = replacements_.tryPop())
        static_cast<void>(retirements_.tryPush(stale));
    }
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
    if (!retired) {
      static_cast<void>(replacements_.tryPush(replacement));
      return;
    }
  }
  static_cast<void>(runtimeLogSink_.beginEpoch());
  activeLogGeneration_.store(replacement->buildGeneration(),
                             std::memory_order_release);
  replacement->attachLogSink(runtimeLogSink_);
  active_ = replacement;
  clearMidiActivity();
  scopeCapture_.requestReset();
  runtimeFaulted_.store(false, std::memory_order_release);
}

std::optional<MidiEvent>
Processor::convertMidi(const juce::MidiMessageMetadata &metadata,
                       const PreparedEngine &engine, const int minimumOffset,
                       const int frames) noexcept {
  const auto &message = metadata.getMessage();
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
      std::clamp(metadata.samplePosition, minimumOffset, frames) -
      minimumOffset);
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
      active_ == nullptr ||
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
      !active_->reset()) {
    deactivateRequested_.store(true, std::memory_order_release);
    runtimeFaulted_.store(true, std::memory_order_release);
    fallback(audio);
    return;
  }

  while (userEvents_.tryPop(userEventScratch_)) {
    if (userEventScratch_.generation != active_->buildGeneration())
      continue;
    if (userEventScratch_.payloadBytes > userEventScratch_.payload.size() ||
        !active_->triggerEvent(
            userEventScratch_.eventIndex,
            {userEventScratch_.payload.data(),
             static_cast<std::size_t>(userEventScratch_.payloadBytes)},
            slotAtomics_, hostContext)) {
      deactivateRequested_.store(true, std::memory_order_release);
      runtimeFaulted_.store(true, std::memory_order_release);
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

  auto succeeded = true;
  auto position = 0;
  for (const auto metadata : midi) {
    observeMidiActivity(metadata.getMessage());
    auto event = convertMidi(metadata, *active_, position, frames);
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
    deactivateRequested_.store(true, std::memory_order_release);
    runtimeFaulted_.store(true, std::memory_order_release);
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
  const auto project = worker_->persistedProjectState();
  return project.projectImage.valid();
}

std::string Processor::saveProjectAs(const juce::File &directory) {
  const auto project = worker_->persistedProjectState();
  if (!project.projectImage.valid())
    return "There is no compiled Onda project to save";
  try {
    auto exported = exportProject(project.projectImage,
                                  pathFromJuce(directory.getFullPathName()));
    if (!exported)
      return exported.error;
    {
      std::lock_guard lock(stateMutex_);
      lastBrowseDirectory_ = exported.projectFile.parent_path();
    }
    worker_->loadWithBufferBindings(exported.projectFile, {}, false);
    return {};
  } catch (const std::bad_alloc &) {
    return "Insufficient memory to export the Onda project";
  } catch (const std::exception &exception) {
    return std::string{"Failed to export the Onda project: "} +
           exception.what();
  }
}

bool Processor::saveProjectAsAsync(
    const juce::File &directory, std::function<void(std::string)> completion) {
  if (exportPending_.exchange(true, std::memory_order_acq_rel))
    return false;
  try {
    exportPool_.addJob([this, directory, completion = std::move(completion)] {
      auto result = saveProjectAs(directory);
      exportPending_.store(false, std::memory_order_release);
      if (completion) {
        juce::MessageManager::callAsync(
            [completion, result = std::move(result)]() mutable {
              completion(std::move(result));
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

void Processor::unload() {
  runtimeFaulted_.store(false, std::memory_order_release);
  runtimeRecoveryRequested_.store(false, std::memory_order_release);
  resetRequested_.store(false, std::memory_order_release);
  activeLogGeneration_.store(0U, std::memory_order_release);
  clearMidiActivity();
  clearRuntimeLog();
  worker_->unload();
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
  for (std::size_t index = 0; index < event->parameters.size(); ++index) {
    if (!appendEventParameter(
            command, event->parameters[index],
            arguments->getReference(static_cast<int>(index)))) {
      return "Event '" + event->name + "' parameter '" +
             event->parameters[index].name + "' has an invalid value";
    }
  }
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
  struct StagedRecord {
    std::uint64_t generation{};
    std::uint64_t epoch{};
    RuntimeLogRecord record;
  };

  std::vector<StagedRecord> records;
  records.reserve(runtimeLogQueueCapacity - 1U);
  RuntimeLogEntry entry;
  for (std::size_t count = 0U;
       count + 1U < runtimeLogQueueCapacity && runtimeLogSink_.tryPop(entry);
       ++count) {
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
  if (!generationChanged && !hasRecords && !hasCounters)
    return;

  {
    std::lock_guard lock(stateMutex_);
    const auto currentGeneration =
        activeLogGeneration_.load(std::memory_order_acquire) == generation;
    const auto currentEpoch = runtimeLogSink_.epoch() == epoch;
    if (!currentGeneration || !currentEpoch)
      return;
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
      while (
          discarded < runtimeLogRecords_.size() &&
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
  }
  if (countersStable) {
    consumedLogEpoch_ = epoch;
    consumedLogCounterTotals_ = counterTotals;
  }
  runtimeLogRevision_.fetch_add(1U, std::memory_order_release);
}

void Processor::timerCallback() {
  drainRuntimeLogs();
  if (auto seed = worker_->takeSeedValues()) {
    if (seed->revision == worker_->requestGeneration()) {
      for (std::size_t index = 0; index < seed->count; ++index)
        setSlotValue(index, seed->values[index]);
      deactivateRequested_.store(false, std::memory_order_release);
    }
    replacementSeedPending_.store(false, std::memory_order_release);
  }
  const auto expected = expectedBlockSize_.load(std::memory_order_acquire);
  if (expected > 0 &&
      expected != configuredBlockSize_.load(std::memory_order_acquire)) {
    for (auto &channel : dryScratch_)
      channel.resize(static_cast<std::size_t>(expected));
    worker_->configure(expectedSampleRate_.load(std::memory_order_acquire),
                       expected);
    configuredBlockSize_.store(expected, std::memory_order_release);
  }
  if (runtimeRecoveryRequested_.exchange(false, std::memory_order_acq_rel))
    worker_->requestRebuild();
}

void Processor::getStateInformation(juce::MemoryBlock &destination) {
  juce::MemoryOutputStream stream(destination, false);
  stream.writeInt(static_cast<int>(stateMagic));
  stream.writeInt(stateVersion);
  const auto project = worker_->persistedProjectState();
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
    stream.writeFloat(normalized(slotValue(index)));
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

    deactivateRequested_.store(true, std::memory_order_release);
    replacementSeedPending_.store(false, std::memory_order_release);
    for (std::size_t index = 0; index < slotCount; ++index)
      setSlotValue(index, restored[index]);
    if (path->isEmpty() && !projectImage.valid()) {
      unload();
    } else {
      if (projectImage.valid()) {
        worker_->restore(pathFromJuce(*path), std::move(projectImage),
                         std::move(bufferBindings));
      } else {
        worker_->loadWithBufferBindings(
            pathFromJuce(*path), std::move(bufferBindings), false,
            Worker::ExistingEnginePolicy::deactivateImmediately);
      }
    }
    editorWidth_.store(std::clamp(width, 360, 1600), std::memory_order_relaxed);
    editorHeight_.store(std::clamp(height, 480, 1400),
                        std::memory_order_relaxed);
    paramControlLayout_.store(layout, std::memory_order_relaxed);
    {
      std::lock_guard lock(stateMutex_);
      lastBrowseDirectory_ = pathFromJuce(*browseDirectory);
    }
  } catch (...) {
    // Host-provided state is untrusted and must never escape the VST callback.
  }
}

} // namespace onda::plugin
