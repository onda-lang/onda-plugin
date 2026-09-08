#include "TemporaryDirectory.h"
#include "TestNumeric.h"
#include "TestWait.h"

#include "AudioFile.h"
#include "Engine.h"
#include "ProjectExport.h"
#include "SpscSlot.h"
#include "TestAudioFile.h"
#include "Worker.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {

class TemporarySource final {
public:
  TemporarySource() {
    path_ = testTemporaryRoot() / "onda-plugin-engine-test.onda";
    dependencyPath_ = path_.parent_path() / "onda_plugin_dependency_test.onda";
    writeValid();
  }

  void writeValid() const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << R"(
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
  }

  void writeInvalid() const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << "this is not valid Onda source\n";
  }

  void writeFaultingEffect() const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << R"(
ins { in1, in2 }
outs { out1, out2 }
params {
  divisor: i32 = 0
}
def quotient(value: i32, by: i32) {
  return value / by
}
sample {
  out1 = f32(quotient(i32(1), divisor))
  out2 = in2
}
)";
  }

  void writeInstrument() const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << R"(
outs { out1 }
init {
  held = 0.0
}
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {
  held = velocity
}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {
  held = 0.0
}
event pitch_bend(channel: i32, value: f32) {
  held = value
}
event channel_pressure(channel: i32, pressure: f32) {
  held = pressure
}
event poly_pressure(channel: i32, key: i32, pressure: f32) {
  held = pressure
}
event cc(channel: i32, index: i32, value: f32) {
  held = value
}
event program_change(channel: i32, program: i32) {
  held = f32(program) / 127.0
}
sample {
  out1 = held
}
)";
  }

  void writeInvalidMidiInstrument() const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << R"(
outs { out1 }
event note_on(key: i32) {
}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {
}
sample {
  out1 = 0.0
}
)";
  }

  void writeParameterAwareInstrument() const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << R"(
outs { out1 }
params {
  amount = 0.25 { 0.0, 1.0 }
}
init {
  held = 0.0
}
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {
  held = amount
}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}
sample {
  out1 = held
}
)";
  }

  void writeEventFaultingInstrument() const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << R"(
outs { out1 }
init {
  held = i32(0)
}
event note_on(id: i32, channel: i32, key: i32, velocity: f32) {
  held = i32(1) / i32(0)
}
event note_off(id: i32, channel: i32, key: i32, velocity: f32) {}
sample {
  out1 = f32(held)
}
)";
  }

  void writeText(const std::string_view source) const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << source;
  }

  void writeDependency(const std::string_view source) const {
    std::ofstream dependency(dependencyPath_,
                             std::ios::binary | std::ios::trunc);
    dependency << source;
  }

  void removeDependency() const {
    std::error_code ignored;
    std::filesystem::remove(dependencyPath_, ignored);
  }

  void writeImportedEntry() const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << R"(
import onda_plugin_dependency_test
ins { in1, in2 }
outs { out1, out2 }
params {
  gain = 0.5 { 0.0, 1.0 }
}
sample {
  out1 = in1 * gain * dependency_gain()
  out2 = in2 * gain * dependency_gain()
}
)";
  }

  void writeImportedEffect(const std::string_view multiplier) const {
    writeDependency("def dependency_gain() { return " +
                    std::string{multiplier} + " }\n");
    writeImportedEntry();
  }

  void writeConstantEffect(const std::string_view value) const {
    writeText("ins { in1, in2 }\nouts { out1, out2 }\nsample {\n"
              "  out1 = " +
              std::string{value} + "\n  out2 = " + std::string{value} +
              "\n}\n");
  }

  void writeBufferEffect() const {
    writeText(R"(
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
  index = index + 1
}
)");
  }

  void writeRuntimeOutputEffect() const {
    writeText(R"(
outs { out1 }
const Bins: f32[2] = [0.25, 0.5]
const Steps: i32[2] = [3, 5]
delegate observed(value: i32, exact: i64, active: bool, bins: f32[2], steps: i32[])
init {
  pin preserved = i32(0)
  transient = i32(0)
  print("init", preserved, transient)
}
sample {
  preserved = preserved + 1
  transient = transient + 1
  print("sample", preserved, transient)
  observed(preserved, i64(9007199254740993), true, Bins, Steps)
  out1 = f32(preserved * 10 + transient)
}
)");
  }

  ~TemporarySource() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    std::filesystem::remove(dependencyPath_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
  std::filesystem::path dependencyPath_;
};

bool close(const float left, const float right) {
  return std::abs(left - right) < 1.0e-5F;
}

bool exerciseDiagnosticOwnership() {
  onda::plugin::OndaDiagnostic diagnostic;
  constexpr std::array<std::uint8_t, 4U> invalidAsset{};
  for (int attempt = 0; attempt < 2; ++attempt) {
    onda_buffer_asset_info_t info{};
    const auto result =
        onda_buffer_asset_decode(invalidAsset.data(), invalidAsset.size(),
                                 &info, nullptr, 0, diagnostic.outParameter());
    if (result >= 0 || diagnostic.copy().empty()) {
      std::cerr << "Onda diagnostic output did not survive safe reuse\n";
      return false;
    }
  }
  return true;
}

onda::plugin::PreparedEngine *waitForReplacement(
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> &replacements) {
  for (const auto deadline =
           std::chrono::steady_clock::now() + test::waitTimeout;
       std::chrono::steady_clock::now() < deadline;) {
    if (auto *engine = replacements.tryPop())
      return engine;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return nullptr;
}

bool produces(onda::plugin::PreparedEngine &engine, const float expected) {
  std::array<std::atomic<float>, onda::plugin::slotCount> values;
  std::array<std::atomic<float> *, onda::plugin::slotCount> slots{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    values[index].store(0.5F);
    slots[index] = &values[index];
  }
  std::array<float, 8U> left{};
  std::array<float, 8U> right{};
  std::array<float *, 2U> outputs{left.data(), right.data()};
  if (!engine.process(nullptr, outputs.data(), 8, {}, slots))
    return false;
  return std::all_of(left.begin(), left.end(),
                     [expected](const float value) {
                       return close(value, expected);
                     }) &&
         std::all_of(right.begin(), right.end(), [expected](const float value) {
           return close(value, expected);
         });
}

std::vector<onda::plugin::RuntimeLogEntry>
drain(onda::plugin::RuntimeLogSink &sink) {
  std::vector<onda::plugin::RuntimeLogEntry> entries;
  onda::plugin::RuntimeLogEntry entry;
  while (sink.tryPop(entry))
    entries.push_back(entry);
  return entries;
}

std::string text(const onda::plugin::RuntimeLogEntry &entry) {
  return {entry.text.data(), entry.textBytes};
}

std::mutex retirementAuditMutex;
std::condition_variable retirementAuditWake;
std::thread::id retirementThread;
int retirementCount{};

void observeRetirement(onda::plugin::PreparedEngine *) noexcept {
  {
    std::lock_guard lock(retirementAuditMutex);
    retirementThread = std::this_thread::get_id();
    ++retirementCount;
  }
  retirementAuditWake.notify_one();
}

bool exerciseExportWriteFailure(const onda::plugin::ProjectImage &image) {
#if defined(__linux__)
  struct FileSizeLimit {
    rlimit previous{};
    using SignalHandler = void (*)(int);
    SignalHandler previousSignal{SIG_ERR};
    bool changed{};

    FileSizeLimit() {
      if (getrlimit(RLIMIT_FSIZE, &previous) != 0)
        return;
      previousSignal = std::signal(SIGXFSZ, SIG_IGN);
      if (previousSignal == SIG_ERR)
        return;
      auto limit = previous;
      limit.rlim_cur = 1;
      changed = setrlimit(RLIMIT_FSIZE, &limit) == 0;
    }
    ~FileSizeLimit() {
      if (changed)
        static_cast<void>(setrlimit(RLIMIT_FSIZE, &previous));
      if (previousSignal != SIG_ERR)
        std::signal(SIGXFSZ, previousSignal);
    }
  };
  for (const auto existing : {false, true}) {
    const auto destination =
        testTemporaryRoot() / (existing ? "limited-empty" : "limited-new");
    if (existing)
      std::filesystem::create_directory(destination);
    FileSizeLimit limit;
    if (!limit.changed) {
      std::cerr << "could not establish the export failure test limit\n";
      return false;
    }
    const auto result = onda::plugin::exportProject(image, destination);
    if (result || result.error.empty() ||
        std::filesystem::exists(destination) != existing ||
        (existing && !std::filesystem::is_empty(destination))) {
      std::cerr << "failed buffered writes published a corrupt export\n";
      return false;
    }
  }
  for (const auto &entry :
       std::filesystem::directory_iterator(testTemporaryRoot())) {
    if (entry.path().filename().string().find(".onda-staging-") !=
        std::string::npos) {
      std::cerr << "failed export left a staging directory\n";
      return false;
    }
  }
#else
  static_cast<void>(image);
#endif
  return true;
}

bool exerciseExportRelinkConflicts(const onda::plugin::ProjectImage &image,
                                   const std::filesystem::path &source) {
  using namespace onda::plugin;
  const auto exported =
      exportProject(image, testTemporaryRoot() / "relink-conflicts");
  if (!exported)
    return false;
  for (const auto action :
       {"unload", "load", "restore", "bind", "clear", "reload", "configure"}) {
    SpscSlot<PreparedEngine *> replacements;
    SpscSlot<PreparedEngine *> retirements;
    std::atomic<bool> deactivate{};
    bool succeeded = false;
    {
      Worker worker(Product::effect, replacements, retirements, deactivate);
      worker.configure(48'000.0, 8);
      worker.restore(source, image, {});
      static_cast<void>(worker.waitForPreparation());
      const auto snapshot = worker.projectExportSnapshot();
      if (!snapshot)
        return false;
      const std::string_view operation(action);
      if (operation == "unload")
        worker.unload();
      else if (operation == "load")
        worker.load(testTemporaryRoot() / "new-selection.onda", false);
      else if (operation == "restore")
        worker.restore({}, image, {});
      else if (operation == "bind")
        worker.bindBufferFile("clip", testTemporaryRoot() / "new-audio.wav");
      else if (operation == "clear")
        worker.clearBuffer("clip");
      else if (operation == "reload")
        worker.requestRebuild();
      else
        worker.configure(44'100.0, 16);
      const auto expectedPath = worker.status().path;
      succeeded = !worker.relinkExport(*snapshot, exported.projectFile) &&
                  worker.status().path == expectedPath;
      if (operation == "load" && worker.projectExportSnapshot())
        succeeded = false;
    }
    delete replacements.tryPop();
    delete retirements.tryPop();
    if (!succeeded) {
      std::cerr << "export replaced a newer " << action << " request\n";
      return false;
    }
  }
  return true;
}

bool exerciseDiskRepairDuringFallback() {
  using namespace onda::plugin;
  TemporarySource source;
  source.writeConstantEffect("0.25");
  const auto initial = PreparedEngine::build(source.path(), Product::effect,
                                             48'000.0, 8);
  if (!initial.engine)
    return false;

  for (const auto fallbackSucceeds : {true, false}) {
    source.writeInvalid();
    SpscSlot<PreparedEngine *> replacements;
    SpscSlot<PreparedEngine *> retirements;
    std::atomic<bool> deactivate{};
    std::mutex gateMutex;
    std::condition_variable gateWake;
    bool fallbackStarted{};
    bool releaseFallback{};
    bool repaired{};
    {
      Worker::ProjectBuildFunction gatedFallback =
          [&](const ProjectImage &image, Product product, double sampleRate,
              int blockSize, const std::optional<ParameterValues> &parameters) {
            {
              std::unique_lock lock(gateMutex);
              fallbackStarted = true;
              gateWake.notify_all();
              if (!gateWake.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return releaseFallback; })) {
                return BuildResult{};
              }
            }
            if (fallbackSucceeds)
              return PreparedEngine::build(image, product, sampleRate,
                                            blockSize, parameters);
            BuildResult failure;
            failure.diagnostic.message = "Injected saved-image build failure";
            return failure;
          };
      Worker worker(Product::effect, replacements, retirements, deactivate,
                    {}, nullptr, {}, std::move(gatedFallback));
      worker.configure(48'000.0, 8);
      worker.restore(source.path(), initial.projectImage, {});
      bool started;
      {
        std::unique_lock lock(gateMutex);
        started = gateWake.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return fallbackStarted; });
      }
      // The disk post-build check is complete; fallback has not finished.
      // This repair must remain visible to the next automatic watcher poll.
      source.writeConstantEffect("0.75");
      {
        std::lock_guard lock(gateMutex);
        releaseFallback = true;
      }
      gateWake.notify_all();
      if (started) {
        for (const auto deadline =
                 std::chrono::steady_clock::now() + test::waitTimeout;
             std::chrono::steady_clock::now() < deadline;) {
          std::unique_ptr<PreparedEngine> replacement{replacements.tryPop()};
          if (replacement && produces(*replacement, 0.75F)) {
            repaired = !worker.status().usingProjectImage;
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
    }
    delete replacements.tryPop();
    delete retirements.tryPop();
    if (!repaired) {
      std::cerr << "disk repair during "
                << (fallbackSucceeds ? "successful" : "failed")
                << " saved-image preparation was not reloaded\n";
      return false;
    }
  }
  return true;
}

} // namespace

int main() {
  if (!exerciseDiagnosticOwnership() || !exerciseDiskRepairDuringFallback())
    return 1;
  auto invalidBytes =
      std::make_shared<const std::vector<std::uint8_t>>(3U, 0xffU);
  onda::plugin::Diagnostic invalidDiagnostic;
  if (onda::plugin::validateProjectImage({.bytes = invalidBytes},
                                         invalidDiagnostic) ||
      invalidDiagnostic.empty()) {
    std::cerr << "invalid Onda project image was accepted\n";
    return 1;
  }

  TemporarySource source;
  auto built = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::effect, 48'000.0, 8);
  if (!built.engine || !built.projectImage.valid()) {
    std::cerr << "build failed: " << built.diagnostic.message << '\n';
    return 1;
  }
  if (!exerciseExportWriteFailure(built.projectImage) ||
      !exerciseExportRelinkConflicts(built.projectImage, source.path()))
    return 1;
  for (std::size_t index = 0;
       index < static_cast<std::size_t>(onda::plugin::MidiKind::count);
       ++index) {
    if (built.engine->handlesMidi(static_cast<onda::plugin::MidiKind>(index))) {
      std::cerr << "effect reported an undeclared MIDI event\n";
      return 1;
    }
  }
  for (std::size_t index = 0;
       index < static_cast<std::size_t>(onda::plugin::HostContextKind::count);
       ++index) {
    if (built.engine->handlesHostContext(
            static_cast<onda::plugin::HostContextKind>(index))) {
      std::cerr << "effect reported an undeclared host-context event\n";
      return 1;
    }
  }
  if (built.engine->needsPositionInfo()) {
    std::cerr << "effect without context events requested playhead data\n";
    return 1;
  }
  const auto mappings = built.engine->parameterMappings();
  if (mappings.size() != 1U || mappings[0].name != "gain" ||
      mappings[0].type != "f32" ||
      !test::withinTolerance(mappings[0].defaultPlain - 0.5, 1.0e-5) ||
      !test::withinTolerance(mappings[0].rangeMin, 1.0e-5) ||
      !test::withinTolerance(mappings[0].rangeMax - 1.0, 1.0e-5) ||
      mappings[0].scale != "linear" || mappings[0].curve || mappings[0].step ||
      mappings[0].stepCount || !mappings[0].unit.empty()) {
    std::cerr << "parameter mapping is incorrect\n";
    return 1;
  }

  std::array<std::atomic<float>, onda::plugin::slotCount> values;
  std::array<std::atomic<float> *, onda::plugin::slotCount> slots{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    values[index].store(index == 0U ? 0.25F : 0.5F);
    slots[index] = &values[index];
  }

  std::array<float, 5U> left{4.0F, 8.0F, 12.0F, 16.0F, 20.0F};
  std::array<float, 5U> right{20.0F, 16.0F, 12.0F, 8.0F, 4.0F};
  std::array<float *, 2U> inputs{left.data(), right.data()};
  std::array<float *, 2U> outputs{left.data(), right.data()};
  if (!built.engine->process(inputs.data(), outputs.data(), 5, {}, slots)) {
    std::cerr << "first process call failed\n";
    return 1;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!close(left[index], static_cast<float>(index + 1U)) ||
        !close(right[index], static_cast<float>(5U - index))) {
      std::cerr << "effect output is incorrect\n";
      return 1;
    }
  }

  left.fill(8.0F);
  right.fill(4.0F);
  values[0].store(0.5F);
  if (!built.engine->process(inputs.data(), outputs.data(), 5, {}, slots)) {
    std::cerr << "boundary-crossing process call failed\n";
    return 1;
  }
  // The first three frames finish the previous logical block with the old
  // snapshot. The remaining two begin a block and observe the new slot value.
  for (std::size_t index = 0; index < 3U; ++index) {
    if (!close(left[index], 2.0F) || !close(right[index], 1.0F))
      return 1;
  }
  for (std::size_t index = 3U; index < 5U; ++index) {
    if (!close(left[index], 4.0F) || !close(right[index], 2.0F))
      return 1;
  }

  built.engine.reset();

  source.writeRuntimeOutputEffect();
  auto runtimeOutput = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::effect, 48'000.0, 8);
  if (!runtimeOutput.engine) {
    std::cerr << "runtime-output effect failed to build: "
              << runtimeOutput.diagnostic.message << '\n';
    return 1;
  }
  onda::plugin::RuntimeLogSink runtimeLog;
  for (std::size_t index = 0;
       index + 1U < onda::plugin::runtimeLogQueueCapacity; ++index) {
    if (!runtimeLog.tryEmplace(
            onda::plugin::RuntimeLogKind::print, 1U,
            [](onda::plugin::RuntimeLogEntry &entry) noexcept {
              entry.text[0] = 'x';
              entry.textBytes = 1U;
              return true;
            })) {
      std::cerr << "runtime log fixture filled too early\n";
      return 1;
    }
  }
  runtimeOutput.engine->attachLogSink(runtimeLog);
  auto logEntries = drain(runtimeLog);
  if (logEntries.size() + 1U != onda::plugin::runtimeLogQueueCapacity) {
    std::cerr << "runtime log fixture was not drained completely\n";
    return 1;
  }

  std::array<float, 1U> loggedLeft{};
  std::array<float, 1U> loggedRight{};
  std::array<float *, 2U> loggedOutputs{loggedLeft.data(), loggedRight.data()};
  if (!runtimeOutput.engine->process(nullptr, loggedOutputs.data(), 1, {},
                                     slots) ||
      !close(loggedLeft[0], 11.0F)) {
    std::cerr << "runtime-output effect did not process its first frame\n";
    return 1;
  }
  logEntries = drain(runtimeLog);
  if (logEntries.size() != 3U ||
      logEntries[0].kind != onda::plugin::RuntimeLogKind::print ||
      text(logEntries[0]) != "init: 0 0" ||
      logEntries[0].sourceFileBytes == 0U ||
      logEntries[0].lexicalOwnerBytes == 0U ||
      logEntries[1].kind != onda::plugin::RuntimeLogKind::print ||
      text(logEntries[1]) != "sample: 1 1" ||
      logEntries[2].kind != onda::plugin::RuntimeLogKind::delegate ||
      text(logEntries[2]) !=
          "delegate observed: value=1 exact=9007199254740993 active=true "
          "bins=[0.25, 0.5] steps=[3, 5]") {
    std::cerr << "pending init and ordered runtime output were not captured\n";
    return 1;
  }

  if (!runtimeOutput.engine->reset(slots)) {
    std::cerr << "preserve-pinned reset failed\n";
    return 1;
  }
  logEntries = drain(runtimeLog);
  if (logEntries.size() != 1U || text(logEntries[0]) != "init: 1 0") {
    std::cerr << "reset init print did not retain pinned state\n";
    return 1;
  }
  loggedLeft[0] = 0.0F;
  loggedRight[0] = 0.0F;
  if (!runtimeOutput.engine->process(nullptr, loggedOutputs.data(), 1, {},
                                     slots) ||
      !close(loggedLeft[0], 21.0F)) {
    std::cerr << "preserve-pinned reset did not retain pinned state\n";
    return 1;
  }
  runtimeOutput.engine.reset();

  auto boundedLog = std::make_unique<onda::plugin::RuntimeLogSink>();
  std::size_t formattedEntries{};
  for (std::size_t index = 0;
       index + 1U < onda::plugin::runtimeLogQueueCapacity; ++index) {
    if (!boundedLog->tryEmplace(
            onda::plugin::RuntimeLogKind::print, 7U,
            [&formattedEntries](onda::plugin::RuntimeLogEntry &entry) noexcept {
              ++formattedEntries;
              entry.text[0] = 'x';
              entry.textBytes = 1U;
              return true;
            })) {
      std::cerr << "bounded runtime log filled too early\n";
      return 1;
    }
  }
  auto formatterCalledWhileFull = false;
  if (boundedLog->tryEmplace(onda::plugin::RuntimeLogKind::print, 7U,
                             [&formatterCalledWhileFull](
                                 onda::plugin::RuntimeLogEntry &) noexcept {
                               formatterCalledWhileFull = true;
                               return true;
                             }) ||
      formatterCalledWhileFull ||
      formattedEntries + 1U != onda::plugin::runtimeLogQueueCapacity) {
    std::cerr << "full runtime log invoked its formatter\n";
    return 1;
  }
  boundedLog->addTransportDrops(onda::plugin::RuntimeLogKind::print,
                                std::numeric_limits<std::uint64_t>::max());
  boundedLog->addTransportDrops(onda::plugin::RuntimeLogKind::print);
  if (boundedLog->counterTotals(boundedLog->epoch()).printTransportDrops !=
      std::numeric_limits<std::uint64_t>::max()) {
    std::cerr << "runtime log counters did not saturate\n";
    return 1;
  }
  boundedLog->addTransportDrops(onda::plugin::RuntimeLogKind::delegate, 2U);
  const auto previousEpoch = boundedLog->epoch();
  const auto firstCounterSnapshot = boundedLog->counterTotals(previousEpoch);
  const auto secondCounterSnapshot = boundedLog->counterTotals(previousEpoch);
  if (firstCounterSnapshot.delegateTransportDrops != 2U ||
      secondCounterSnapshot.delegateTransportDrops != 2U) {
    std::cerr << "runtime log counter snapshot was destructive\n";
    return 1;
  }
  static_cast<void>(boundedLog->beginEpoch());
  boundedLog->addTransportDrops(onda::plugin::RuntimeLogKind::delegate, 3U);
  if (boundedLog->counterTotals(previousEpoch).delegateTransportDrops != 0U ||
      boundedLog->counterTotals(boundedLog->epoch()).delegateTransportDrops !=
          3U) {
    std::cerr << "runtime log counters crossed an epoch boundary\n";
    return 1;
  }

  onda::plugin::RuntimeLogSink epochLog;
  const auto entryEpoch = epochLog.epoch();
  if (!epochLog.tryEmplace(
          onda::plugin::RuntimeLogKind::print, 8U,
          [&epochLog](onda::plugin::RuntimeLogEntry &entry) noexcept {
            static_cast<void>(epochLog.beginEpoch());
            entry.text[0] = 'x';
            entry.textBytes = 1U;
            return true;
          })) {
    std::cerr << "runtime log epoch fixture was not published\n";
    return 1;
  }
  auto epochEntries = drain(epochLog);
  if (epochEntries.size() != 1U || epochEntries[0].epoch != entryEpoch) {
    std::cerr << "in-flight runtime output crossed an epoch boundary\n";
    return 1;
  }

  source.writeFaultingEffect();
  auto faulting = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::effect, 48'000.0, 8);
  if (!faulting.engine) {
    std::cerr << "faulting effect should build: " << faulting.diagnostic.message
              << '\n';
    return 1;
  }
  left.fill(1.0F);
  right.fill(1.0F);
  if (faulting.engine->process(inputs.data(), outputs.data(), 1, {}, slots)) {
    std::cerr << "generated runtime failure was reported as success\n";
    return 1;
  }
  faulting.engine.reset();

  source.writeInstrument();
  auto instrument = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::instrument, 48'000.0, 8);
  if (!instrument.engine) {
    std::cerr << "instrument build failed: " << instrument.diagnostic.message
              << '\n';
    return 1;
  }
  for (std::size_t index = 0;
       index < static_cast<std::size_t>(onda::plugin::MidiKind::count);
       ++index) {
    if (!instrument.engine->handlesMidi(
            static_cast<onda::plugin::MidiKind>(index))) {
      std::cerr << "instrument omitted a declared MIDI event binding\n";
      return 1;
    }
  }
  std::array<float, 8U> instrumentLeft{};
  std::array<float, 8U> instrumentRight{};
  std::array<float *, 2U> instrumentOutputs{instrumentLeft.data(),
                                            instrumentRight.data()};
  const std::array<onda::plugin::MidiEvent, 2U> events{{
      {
          .kind = onda::plugin::MidiKind::noteOn,
          .sampleOffset = 2,
          .channel = 0,
          .keyOrController = 60,
          .value = 0.75F,
      },
      {
          .kind = onda::plugin::MidiKind::noteOff,
          .sampleOffset = 5,
          .channel = 0,
          .keyOrController = 60,
          .value = 0.0F,
      },
  }};
  if (!instrument.engine->process(nullptr, instrumentOutputs.data(), 8, events,
                                  slots)) {
    std::cerr << "instrument MIDI process call failed\n";
    return 1;
  }
  for (std::size_t index = 0; index < instrumentLeft.size(); ++index) {
    const auto expected = index >= 2U && index < 5U ? 0.75F : 0.0F;
    if (!close(instrumentLeft[index], expected) ||
        !close(instrumentRight[index], 0.0F)) {
      std::cerr << "instrument MIDI scheduling is incorrect\n";
      return 1;
    }
  }

  if (!instrument.engine->reset(slots)) {
    std::cerr << "instrument reset failed\n";
    return 1;
  }
  instrumentLeft.fill(0.0F);
  instrumentRight.fill(0.0F);
  const std::array<onda::plugin::MidiEvent, 5U> controllerEvents{{
      {
          .kind = onda::plugin::MidiKind::pitchBend,
          .sampleOffset = 1,
          .channel = 2,
          .value = 0.25F,
      },
      {
          .kind = onda::plugin::MidiKind::polyPressure,
          .sampleOffset = 3,
          .channel = 2,
          .keyOrController = 60,
          .value = 0.375F,
      },
      {
          .kind = onda::plugin::MidiKind::channelPressure,
          .sampleOffset = 4,
          .channel = 2,
          .value = 0.5F,
      },
      {
          .kind = onda::plugin::MidiKind::controlChange,
          .sampleOffset = 5,
          .channel = 2,
          .keyOrController = 74,
          .value = 0.75F,
      },
      {
          .kind = onda::plugin::MidiKind::programChange,
          .sampleOffset = 7,
          .channel = 2,
          .keyOrController = 63,
      },
  }};
  if (!instrument.engine->process(nullptr, instrumentOutputs.data(), 8,
                                  controllerEvents, slots)) {
    std::cerr << "instrument controller process call failed\n";
    return 1;
  }
  for (std::size_t index = 0; index < instrumentLeft.size(); ++index) {
    const auto expected = index < 1U   ? 0.0F
                          : index < 3U ? 0.25F
                          : index < 4U ? 0.375F
                          : index < 5U ? 0.5F
                          : index < 7U ? 0.75F
                                       : 63.0F / 127.0F;
    if (!close(instrumentLeft[index], expected) ||
        !close(instrumentRight[index], 0.0F)) {
      std::cerr << "instrument controller scheduling is incorrect\n";
      return 1;
    }
  }

  if (!instrument.engine->reset(slots)) {
    std::cerr << "instrument reset failed\n";
    return 1;
  }
  instrumentLeft.fill(1.0F);
  instrumentRight.fill(1.0F);
  if (!instrument.engine->process(nullptr, instrumentOutputs.data(), 6, {},
                                  slots)) {
    std::cerr << "logical-boundary setup process failed\n";
    return 1;
  }
  const std::array<onda::plugin::MidiEvent, 2U> equalOffsetEvents{{
      {
          .kind = onda::plugin::MidiKind::noteOn,
          .sampleOffset = 2,
          .channel = 0,
          .keyOrController = 60,
          .value = 0.25F,
      },
      {
          .kind = onda::plugin::MidiKind::controlChange,
          .sampleOffset = 2,
          .channel = 0,
          .keyOrController = 1,
          .value = 0.75F,
      },
  }};
  if (!instrument.engine->process(nullptr, instrumentOutputs.data(), 4,
                                  equalOffsetEvents, slots)) {
    std::cerr << "equal-offset boundary MIDI process failed\n";
    return 1;
  }
  for (std::size_t index = 0; index < 4U; ++index) {
    const auto expected = index < 2U ? 0.0F : 0.75F;
    if (!close(instrumentLeft[index], expected)) {
      std::cerr << "equal-offset MIDI order or logical-boundary timing is "
                   "incorrect\n";
      return 1;
    }
  }
  instrument.engine.reset();

  source.writeParameterAwareInstrument();
  auto parameterAwareInstrument = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::instrument, 48'000.0, 8);
  if (!parameterAwareInstrument.engine) {
    std::cerr << "parameter-aware instrument build failed: "
              << parameterAwareInstrument.diagnostic.message << '\n';
    return 1;
  }
  values[0].store(0.75F);
  instrumentLeft.fill(0.0F);
  instrumentRight.fill(0.0F);
  const std::array parameterBoundaryEvent{onda::plugin::MidiEvent{
      .kind = onda::plugin::MidiKind::noteOn,
      .sampleOffset = 0,
      .channel = 0,
      .keyOrController = 60,
      .value = 1.0F,
  }};
  if (!parameterAwareInstrument.engine->process(
          nullptr, instrumentOutputs.data(), 1, parameterBoundaryEvent,
          slots) ||
      !close(instrumentLeft[0], 0.75F)) {
    std::cerr << "boundary MIDI did not observe the current logical-block "
                 "parameters\n";
    return 1;
  }
  parameterAwareInstrument.engine.reset();

  source.writeInvalidMidiInstrument();
  const auto invalidInstrument = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::instrument, 48'000.0, 8);
  if (invalidInstrument.engine ||
      invalidInstrument.diagnostic.message.find("Canonical MIDI event") ==
          std::string::npos) {
    std::cerr << "incompatible canonical MIDI event was accepted\n";
    return 1;
  }

  source.writeEventFaultingInstrument();
  auto eventFaulting = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::instrument, 48'000.0, 8);
  if (!eventFaulting.engine) {
    std::cerr << "event-faulting instrument should build: "
              << eventFaulting.diagnostic.message << '\n';
    return 1;
  }
  const std::array eventFault{onda::plugin::MidiEvent{
      .kind = onda::plugin::MidiKind::noteOn,
      .sampleOffset = 0,
      .channel = 0,
      .keyOrController = 60,
      .value = 1.0F,
  }};
  if (eventFaulting.engine->process(nullptr, instrumentOutputs.data(), 1,
                                    eventFault, slots)) {
    std::cerr << "generated event runtime failure was reported as success\n";
    return 1;
  }
  eventFaulting.engine.reset();

  const auto expectRejected =
      [&source](const std::string_view text,
                const std::string_view expectedDiagnostic) {
        source.writeText(text);
        const auto rejected = onda::plugin::PreparedEngine::build(
            source.path(), onda::plugin::Product::effect, 48'000.0, 8);
        return !rejected.engine && rejected.diagnostic.message.find(
                                       expectedDiagnostic) != std::string::npos;
      };
  if (!expectRejected("ins { in1, in2, in3 }\nouts { out1, out2 }\n"
                      "sample { out1 = in1; out2 = in2 }\n",
                      "at most 2 f32 channels") ||
      !expectRejected("ins { in1: f64 }\nouts { out1 }\n"
                      "sample { out1 = f32(in1) }\n",
                      "at most 2 f32 channels")) {
    std::cerr << "unsupported plugin interface was not rejected\n";
    return 1;
  }
  if (!expectRejected("ins { in1, in2 }\nouts { out1, out2 }\n"
                      "event tempo(bpm: f32) {}\n"
                      "sample { out1 = in1; out2 = in2 }\n",
                      "Canonical host-context event")) {
    std::cerr << "incompatible canonical host-context event was accepted\n";
    return 1;
  }
  if (!expectRejected("outs { out1 }\nevent tempo(bpm: f64[1]) {}\n"
                      "sample { out1 = 0.0 }\n",
                      "Canonical host-context event") ||
      !expectRejected("outs { out1 }\n"
                      "event note_on(id: i32[1], channel: i32, key: i32, "
                      "velocity: f32) {}\n"
                      "sample { out1 = 0.0 }\n",
                      "Canonical MIDI event")) {
    std::cerr << "one-element arrays were accepted as canonical scalars\n";
    return 1;
  }

  source.writeText(R"(
ins { in1, in2 }
outs { out1, out2 }
event tempo(bpm: f64) {}
sample { out1 = in1; out2 = in2 }
)");
  auto tempoOnly = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::effect, 48'000.0, 8);
  if (!tempoOnly.engine ||
      !tempoOnly.engine->handlesHostContext(
          onda::plugin::HostContextKind::tempo) ||
      !tempoOnly.engine->needsPositionInfo()) {
    std::cerr << "declared tempo metadata was not retained by the engine\n";
    return 1;
  }
  for (std::size_t index = 0;
       index < static_cast<std::size_t>(onda::plugin::HostContextKind::count);
       ++index) {
    const auto kind = static_cast<onda::plugin::HostContextKind>(index);
    if (kind != onda::plugin::HostContextKind::tempo &&
        tempoOnly.engine->handlesHostContext(kind)) {
      std::cerr << "tempo-only engine reported an undeclared context event\n";
      return 1;
    }
  }
  tempoOnly.engine.reset();

  TemporaryAudioFile audioFile{"onda-plugin-buffer-engine-test"};
  if (!audioFile.write(0.1F, 0.5F) ||
      onda::plugin::supportedAudioFileWildcard().find("*.flac") ==
          std::string::npos) {
    std::cerr << "JUCE FLAC support is unavailable\n";
    return 1;
  }
  source.writeBufferEffect();
  const auto unboundBuffer = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::effect, 48'000.0, 8);
  if (unboundBuffer.engine ||
      unboundBuffer.diagnostic.message != "Buffer 'clip' is not bound" ||
      unboundBuffer.buffers.size() != 1U ||
      unboundBuffer.buffers[0].channelKind !=
          onda::plugin::BufferChannelKind::fixed ||
      unboundBuffer.buffers[0].fixedChannels != 2) {
    std::cerr << "unbound buffer metadata or diagnostic is incorrect: "
              << unboundBuffer.diagnostic.message << " ("
              << unboundBuffer.buffers.size() << " buffers)\n";
    return 1;
  }

  const std::array bufferBindings{onda::plugin::BufferFileBinding{
      .name = "clip", .path = audioFile.path()}};
  auto buffered = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::effect, 48'000.0, 8,
      bufferBindings);
  if (!buffered.engine || buffered.engine->bufferMappings().size() != 1U ||
      !buffered.projectImage.valid()) {
    std::cerr << "FLAC-backed buffer build failed: "
              << buffered.diagnostic.message << '\n';
    return 1;
  }
  const auto &bufferMetadata = buffered.engine->bufferMappings()[0];
  if (bufferMetadata.loadedPath != audioFile.path() ||
      bufferMetadata.loadedFrames != 8 || bufferMetadata.loadedChannels != 2 ||
      !close(bufferMetadata.loadedSampleRate, 48'000.0F)) {
    std::cerr << "decoded buffer metadata is incorrect\n";
    return 1;
  }

  std::array<float, 8U> bufferLeft{};
  std::array<float, 8U> bufferRight{};
  std::array<float *, 2U> bufferOutputs{bufferLeft.data(), bufferRight.data()};
  if (!buffered.engine->process(nullptr, bufferOutputs.data(), 8, {}, slots)) {
    std::cerr << "buffer-backed processing failed\n";
    return 1;
  }
  for (std::size_t index = 0; index < bufferLeft.size(); ++index) {
    const auto offset = static_cast<float>(index) * 0.01F;
    if (!test::withinTolerance(bufferLeft[index] - (0.1F + offset), 5.0e-4F) ||
        !test::withinTolerance(bufferRight[index] - (0.5F + offset), 5.0e-4F)) {
      std::cerr << "decoded interleaved buffer data is incorrect\n";
      return 1;
    }
  }

  if (!audioFile.write(0.2F, 0.6F))
    return 1;
  auto restoredBuffer = onda::plugin::PreparedEngine::build(
      buffered.projectImage, onda::plugin::Product::effect, 48'000.0, 8);
  std::ranges::fill(bufferLeft, 0.0F);
  std::ranges::fill(bufferRight, 0.0F);
  if (!restoredBuffer.engine ||
      !restoredBuffer.engine->process(nullptr, bufferOutputs.data(), 8, {},
                                      slots) ||
      !test::withinTolerance(bufferLeft[0] - 0.1F, 5.0e-4F) ||
      !test::withinTolerance(bufferRight[0] - 0.5F, 5.0e-4F)) {
    std::cerr << "project image did not restore independently of disk\n";
    return 1;
  }

  const auto assetExportRoot =
      testTemporaryRoot() /
      ("onda-buffer-export-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(assetExportRoot);
  const auto exportedBufferProject =
      onda::plugin::exportProject(buffered.projectImage, assetExportRoot);
  if (!exportedBufferProject ||
      !std::filesystem::is_regular_file(exportedBufferProject.projectFile)) {
    std::cerr << "portable buffer project export failed: "
              << exportedBufferProject.error << '\n';
    std::filesystem::remove_all(assetExportRoot);
    return 1;
  }
  auto exportedBufferBuild = onda::plugin::PreparedEngine::build(
      exportedBufferProject.projectFile, onda::plugin::Product::effect,
      48'000.0, 8);
  std::ranges::fill(bufferLeft, 0.0F);
  std::ranges::fill(bufferRight, 0.0F);
  if (!exportedBufferBuild.engine ||
      !exportedBufferBuild.engine->process(nullptr, bufferOutputs.data(), 8, {},
                                           slots) ||
      !test::withinTolerance(bufferLeft[0] - 0.1F, 5.0e-4F) ||
      !test::withinTolerance(bufferRight[0] - 0.5F, 5.0e-4F)) {
    std::cerr << "exported buffer asset did not reproduce its checkpoint: "
              << exportedBufferBuild.diagnostic.message << " (samples "
              << bufferLeft[0] << ", " << bufferRight[0] << ")\n";
    std::filesystem::remove_all(assetExportRoot);
    return 1;
  }
  std::filesystem::remove_all(assetExportRoot);
  buffered.engine.reset();

  for (const auto writer : {
           "sample { clip[0, 0] = 0.5; out1 = clip[0, 0] }",
           "init { clip[0, 0] = 0.5 }\nsample { out1 = clip[0, 0] }",
           "event overwrite() { clip[0, 0] = 0.5 }\n"
           "sample { out1 = clip[0, 0] }"}) {
    source.writeText(std::string{"buffers { clip: buffer<f32[2]> }\n"
                                 "outs { out1 }\n"} + writer);
    const auto writable = onda::plugin::PreparedEngine::build(
        source.path(), onda::plugin::Product::effect, 48'000.0, 8,
        bufferBindings);
    if (writable.engine || writable.projectImage.valid() ||
        writable.diagnostic.message.find("must be read-only") ==
            std::string::npos ||
        std::ranges::find(writable.watchPaths, audioFile.path()) ==
            writable.watchPaths.end()) {
      std::cerr << "writable audio-file binding was not rejected: "
                << writable.diagnostic.message << '\n';
      return 1;
    }
  }

  source.writeText(
      "buffers { clip: buffer<f32> }\nouts { out1 }\nsample { out1 = 0.0 }\n");
  const auto wrongChannels = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::effect, 48'000.0, 8,
      bufferBindings);
  if (wrongChannels.engine || wrongChannels.diagnostic.message.find(
                                  "expected 1") == std::string::npos) {
    std::cerr << "incompatible audio-file channel count was accepted\n";
    return 1;
  }
  source.writeText(
      "buffers { clip: buffer<f64> }\nouts { out1 }\nsample { out1 = 0.0 }\n");
  const auto wrongType = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::effect, 48'000.0, 8,
      bufferBindings);
  if (wrongType.engine ||
      wrongType.diagnostic.message.find("require an f32 Onda buffer") ==
          std::string::npos) {
    std::cerr << "incompatible audio-file buffer type was accepted\n";
    return 1;
  }

  source.writeImportedEffect("0.25");
  onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> replacements;
  onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> retirements;
  std::atomic<bool> deactivate{};
  onda::plugin::PreparedEngine *workerEngine{};
  auto handoffBlocker = onda::plugin::PreparedEngine::build(
      source.path(), onda::plugin::Product::effect, 48'000.0, 8);
  if (!handoffBlocker.engine ||
      !replacements.tryPush(handoffBlocker.engine.release())) {
    std::cerr << "could not occupy the replacement handoff for supersession "
                 "coverage\n";
    return 1;
  }
  {
    onda::plugin::Worker worker(onda::plugin::Product::effect, replacements,
                                retirements, deactivate);
    worker.configure(48'000.0, 8);
    worker.load(source.path(), true);

    // The worker must replace and destroy an unconsumed engine itself; a
    // suspended host cannot drain the slot to unblock host preparation.
    const auto preparedGeneration = worker.waitForPreparation();
    if (preparedGeneration != worker.requestGeneration() ||
        !worker.status().active) {
      std::cerr << "worker did not replace an unconsumed handoff\n";
      return 1;
    }

    workerEngine = waitForReplacement(replacements);
    if (workerEngine == nullptr || worker.takeSeedValues()) {
      std::cerr << "unadopted engine exposed host defaults\n";
      return 1;
    }
    worker.setActiveGeneration(workerEngine->buildGeneration());
    const auto seed = worker.takeSeedValues();
    if (deactivate.load() || !seed || seed->count != 1U ||
        seed->revision != worker.requestGeneration() ||
        workerEngine->buildGeneration() != worker.requestGeneration() ||
        !close(seed->values[0], 0.5F)) {
      std::cerr << "worker did not publish the initial engine\n";
      return 1;
    }
    worker.finishSeeding(seed);

    source.writeImportedEffect("0.75");
    auto *dependencyReload = waitForReplacement(replacements);
    if (dependencyReload == nullptr) {
      std::cerr << "transitive dependency edit did not trigger a reload\n";
      return 1;
    }
    if (!retirements.tryPush(workerEngine)) {
      std::cerr << "could not retire the engine after dependency reload\n";
      return 1;
    }
    workerEngine = dependencyReload;
    worker.setActiveGeneration(workerEngine->buildGeneration());

    worker.configure(44'100.0, 16);
    auto *reconfigured = waitForReplacement(replacements);
    if (reconfigured == nullptr || reconfigured->sampleRate() != 44'100.0 ||
        reconfigured->blockSize() != 16) {
      std::cerr << "worker did not rebuild for the host audio configuration\n";
      return 1;
    }
    if (!retirements.tryPush(workerEngine)) {
      std::cerr << "could not retire the engine after host reconfiguration\n";
      return 1;
    }
    workerEngine = reconfigured;
    worker.setActiveGeneration(workerEngine->buildGeneration());

    source.writeInvalid();
    bool observedFailure = false;
    for (const auto deadline =
             std::chrono::steady_clock::now() + test::waitTimeout;
         std::chrono::steady_clock::now() < deadline;) {
      const auto status = worker.status();
      if (!status.compiling && status.message != "Active" &&
          status.message != "Waiting to compile") {
        observedFailure = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!observedFailure || replacements.tryPop() != nullptr) {
      std::cerr << "failed reload did not retain the last good engine\n";
      return 1;
    }

    source.writeValid();
    auto *replacement = waitForReplacement(replacements);
    if (replacement == nullptr) {
      std::cerr << "worker did not recover after the source was repaired\n";
      return 1;
    }
    if (!retirements.tryPush(workerEngine)) {
      std::cerr << "could not retire the previous engine\n";
      return 1;
    }
    workerEngine = replacement;
    worker.setActiveGeneration(workerEngine->buildGeneration());

    const auto revisionWithoutDependency = worker.status().revision;
    source.removeDependency();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    if (replacements.tryPop() != nullptr ||
        worker.status().revision != revisionWithoutDependency) {
      std::cerr << "obsolete dependency remained in the watch set\n";
      return 1;
    }

    worker.unload();
    if (!deactivate.load()) {
      std::cerr << "unload did not request realtime deactivation\n";
      return 1;
    }
  }
  delete workerEngine;

  source.removeDependency();
  source.writeImportedEntry();
  onda::plugin::PreparedEngine *unresolvedRecovery{};
  {
    onda::plugin::Worker worker(onda::plugin::Product::effect, replacements,
                                retirements, deactivate);
    deactivate.store(false);
    worker.configure(48'000.0, 8);
    worker.load(source.path(), false);
    bool observedMissingDependency = false;
    for (const auto deadline =
             std::chrono::steady_clock::now() + test::waitTimeout;
         std::chrono::steady_clock::now() < deadline;) {
      const auto status = worker.status();
      if (!status.compiling && status.message != "Waiting to compile" &&
          status.message != "Waiting for host specialization") {
        observedMissingDependency = !status.active;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!observedMissingDependency || replacements.tryPop() != nullptr) {
      std::cerr << "missing dependency did not fail safely\n";
      return 1;
    }

    source.writeDependency("def dependency_gain() { return 0.5 }\n");
    unresolvedRecovery = waitForReplacement(replacements);
    if (unresolvedRecovery == nullptr) {
      std::cerr << "creating an unresolved dependency did not recover\n";
      return 1;
    }
  }
  delete unresolvedRecovery;

  {
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> failureReplacements;
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> failureRetirements;
    std::atomic<bool> failureDeactivate{};
    onda::plugin::Worker::BuildFunction failOutsideBuildBoundary =
        [](const std::filesystem::path &, const onda::plugin::Product, double,
           int, std::span<const onda::plugin::BufferFileBinding>,
           const std::optional<onda::plugin::ParameterValues> &)
        -> onda::plugin::BuildResult { throw 7; };
    onda::plugin::Worker worker(onda::plugin::Product::effect,
                                failureReplacements, failureRetirements,
                                failureDeactivate,
                                std::move(failOutsideBuildBoundary));
    worker.configure(48'000.0, 8);
    worker.load(source.path(), false);

    bool failureContained = false;
    for (const auto deadline =
             std::chrono::steady_clock::now() + test::waitTimeout;
         std::chrono::steady_clock::now() < deadline;) {
      const auto status = worker.status();
      if (!status.compiling &&
          status.message == "Onda worker stopped after an unknown failure") {
        failureContained = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!failureContained || failureReplacements.tryPop() != nullptr) {
      std::cerr << "worker thread did not contain an unexpected exception\n";
      return 1;
    }
  }

  source.writeConstantEffect("0.25");
  {
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> raceReplacements;
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> raceRetirements;
    std::atomic<bool> raceDeactivate{};
    std::mutex gateMutex;
    std::condition_variable gateWake;
    int buildCalls{};
    bool firstResultReady{};
    bool releaseFirst{};
    bool secondStarted{};
    bool releaseSecond{};

    onda::plugin::Worker::BuildFunction gatedBuild =
        [&](const std::filesystem::path &path,
            const onda::plugin::Product product, const double sampleRate,
            const int blockSize,
            const std::span<const onda::plugin::BufferFileBinding> bindings,
            const std::optional<onda::plugin::ParameterValues> &parameters) {
          int call{};
          {
            std::lock_guard lock(gateMutex);
            call = ++buildCalls;
          }
          if (call == 1) {
            auto result = onda::plugin::PreparedEngine::build(
                path, product, sampleRate, blockSize, bindings, parameters);
            std::unique_lock lock(gateMutex);
            firstResultReady = true;
            gateWake.notify_all();
            if (!gateWake.wait_for(lock, test::waitTimeout,
                                   [&] { return releaseFirst; }))
              return onda::plugin::BuildResult{};
            return result;
          }
          if (call == 2) {
            std::unique_lock lock(gateMutex);
            secondStarted = true;
            gateWake.notify_all();
            if (!gateWake.wait_for(lock, test::waitTimeout,
                                   [&] { return releaseSecond; }))
              return onda::plugin::BuildResult{};
          }
          return onda::plugin::PreparedEngine::build(
              path, product, sampleRate, blockSize, bindings, parameters);
        };

    onda::plugin::Worker worker(onda::plugin::Product::effect, raceReplacements,
                                raceRetirements, raceDeactivate,
                                std::move(gatedBuild));
    worker.configure(48'000.0, 8);
    worker.load(source.path(), false);
    {
      std::unique_lock lock(gateMutex);
      if (!gateWake.wait_for(lock, std::chrono::seconds(5),
                             [&] { return firstResultReady; })) {
        std::cerr << "deterministic reload race did not start\n";
        return 1;
      }
    }
    source.writeConstantEffect("0.75");
    {
      std::lock_guard lock(gateMutex);
      releaseFirst = true;
    }
    gateWake.notify_all();
    {
      std::unique_lock lock(gateMutex);
      if (!gateWake.wait_for(lock, std::chrono::seconds(5),
                             [&] { return secondStarted; })) {
        std::cerr << "source change during compilation was not retried\n";
        return 1;
      }
      if (raceReplacements.tryPop() != nullptr) {
        std::cerr << "stale compile result reached the realtime handoff\n";
        return 1;
      }
      releaseSecond = true;
    }
    gateWake.notify_all();
    std::unique_ptr<onda::plugin::PreparedEngine> replacement{
        waitForReplacement(raceReplacements)};
    if (!replacement || !produces(*replacement, 0.75F)) {
      std::cerr << "reload race did not publish the newest source\n";
      return 1;
    }
  }

  source.writeConstantEffect("0.5");
  {
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *>
        checkpointReplacements;
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *>
        checkpointRetirements;
    std::atomic<bool> checkpointDeactivate{};
    onda::plugin::Worker worker(onda::plugin::Product::effect,
                                checkpointReplacements, checkpointRetirements,
                                checkpointDeactivate);
    worker.configure(48'000.0, 8);
    worker.load(source.path(), false);
    std::unique_ptr<onda::plugin::PreparedEngine> engine{
        waitForReplacement(checkpointReplacements)};
    const auto checkpoint = worker.persistedProjectState();
    if (!engine || checkpoint.path != source.path() ||
        !checkpoint.projectImage.valid()) {
      std::cerr << "worker did not publish a complete project checkpoint\n";
      return 1;
    }

    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto invalidPath =
        testTemporaryRoot() /
        ("onda-plugin-invalid-checkpoint-" + std::to_string(stamp) + ".onda");
    {
      std::ofstream invalid(invalidPath, std::ios::binary | std::ios::trunc);
      invalid << "not valid Onda source\n";
    }
    worker.load(invalidPath, false);
    const auto pending = worker.persistedProjectState();
    if (pending.path != checkpoint.path ||
        pending.bufferBindings != checkpoint.bufferBindings ||
        pending.projectImage != checkpoint.projectImage) {
      std::cerr << "pending request contaminated the saved checkpoint\n";
      return 1;
    }
    bool failed = false;
    for (const auto deadline =
             std::chrono::steady_clock::now() + test::waitTimeout;
         std::chrono::steady_clock::now() < deadline;) {
      const auto status = worker.status();
      if (status.path == invalidPath && !status.compiling &&
          status.message != "Waiting to compile") {
        failed = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto afterFailure = worker.persistedProjectState();
    if (!failed || afterFailure.path != checkpoint.path ||
        afterFailure.projectImage != checkpoint.projectImage) {
      std::cerr << "failed request replaced the last good checkpoint\n";
      return 1;
    }
    std::error_code ignored;
    std::filesystem::remove(invalidPath, ignored);
  }

  // Claim B while A is still reported as active, then finish adoption while
  // C is compiling. A failure must recover B's checkpoint and interface even
  // though C's initial status update observed A.
  source.writeConstantEffect("0.25");
  {
    using namespace onda::plugin;
    SpscSlot<PreparedEngine *> publications;
    SpscSlot<PreparedEngine *> retired;
    std::atomic<bool> deactivate{};
    std::mutex gateMutex;
    std::condition_variable gateWake;
    bool blockBuild{}, entered{}, release{};
    Worker worker(Product::effect, publications, retired, deactivate,
                  [&](const auto &path, const auto product, const auto rate,
                      const auto blockSize, const auto bindings,
                      const auto &parameters) {
                    {
                      std::unique_lock lock(gateMutex);
                      if (blockBuild) {
                        entered = true;
                        gateWake.notify_one();
                        if (!gateWake.wait_for(lock, test::waitTimeout,
                                               [&] { return release; }))
                          return onda::plugin::BuildResult{};
                      }
                    }
                    return PreparedEngine::build(path, product, rate, blockSize,
                                                 bindings, parameters);
                  });
    worker.configure(48'000.0, 8);
    worker.load(source.path(), false);
    static_cast<void>(worker.waitForPreparation());
    std::unique_ptr<PreparedEngine> first{publications.tryPop()};
    if (!first)
      return 1;
    worker.setActiveGeneration(first->buildGeneration());
    source.writeConstantEffect("0.75");
    worker.load(source.path(), false);
    static_cast<void>(worker.waitForPreparation());
    std::unique_ptr<PreparedEngine> second{publications.tryPop()};
    if (!second)
      return 1;
    const auto secondProject = worker.persistedProjectState();
    {
      std::lock_guard lock(gateMutex);
      blockBuild = true;
    }
    source.writeInvalid();
    worker.load(source.path(), false);
    bool started{};
    {
      std::unique_lock lock(gateMutex);
      started = gateWake.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return entered; });
      worker.setActiveGeneration(second->buildGeneration());
      first.reset();
      release = true;
    }
    gateWake.notify_one();
    if (!started)
      return 1;
    static_cast<void>(worker.waitForPreparation());
    const auto retained = worker.persistedProjectState();
    auto restored = PreparedEngine::build(retained.projectImage,
                                          Product::effect, 48'000.0, 8);
    if (worker.status().engineGeneration != second->buildGeneration() ||
        retained.projectImage != secondProject.projectImage ||
        !restored.engine || !produces(*restored.engine, 0.75F)) {
      std::cerr << "late adoption lost its interface or checkpoint\n";
      return 1;
    }
  }

  for (const bool withDependency : {false, true}) {
    if (withDependency) {
      source.writeDependency("def dependency_gain() { return 0.5 }\n");
      source.writeText("import onda_plugin_dependency_test\n"
                       "outs { out1, out2 }\n"
                       "sample { out1 = dependency_gain(); "
                       "out2 = dependency_gain() }\n");
    } else {
      source.writeConstantEffect("0.5");
    }
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> watchReplacements;
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> watchRetirements;
    std::atomic<bool> watchDeactivate{};
    std::atomic<int> buildCalls{};
    onda::plugin::Worker::BuildFunction countedBuild =
        [&buildCalls](
            const std::filesystem::path &path,
            const onda::plugin::Product product, const double sampleRate,
            const int blockSize,
            const std::span<const onda::plugin::BufferFileBinding> bindings,
            const std::optional<onda::plugin::ParameterValues> &parameters) {
          buildCalls.fetch_add(1, std::memory_order_relaxed);
          return onda::plugin::PreparedEngine::build(
              path, product, sampleRate, blockSize, bindings, parameters);
        };
    onda::plugin::Worker worker(
        onda::plugin::Product::effect, watchReplacements, watchRetirements,
        watchDeactivate, std::move(countedBuild));
    worker.configure(48'000.0, 8);
    worker.load(source.path(), false);
    std::unique_ptr<onda::plugin::PreparedEngine> initial{
        waitForReplacement(watchReplacements)};
    // Initial compilation may discover imports or canonical path aliases and
    // retry to validate the expanded watch set. Measure edits after publication.
    const auto initialBuildCalls = buildCalls.load(std::memory_order_relaxed);
    if (!initial || !produces(*initial, 0.5F)) {
      std::cerr << "metadata-gated watcher did not publish the initial program "
                << "(imported " << withDependency << ", build calls "
                << initialBuildCalls << ", status " << worker.status().message
                << ")\n";
      return 1;
    }

    std::error_code timestampError;
    const auto previousTime =
        std::filesystem::last_write_time(source.path(), timestampError);
    if (timestampError) {
      std::cerr << "could not inspect source timestamp for watcher coverage\n";
      return 1;
    }
    std::filesystem::last_write_time(
        source.path(), previousTime + std::chrono::seconds(2), timestampError);
    if (timestampError) {
      std::cerr << "could not touch source timestamp for watcher coverage\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    if (buildCalls.load(std::memory_order_relaxed) != initialBuildCalls ||
        watchReplacements.tryPop() != nullptr) {
      std::cerr << "timestamp-only edit triggered an unnecessary rebuild "
                << "(initial " << initialBuildCalls << ", current "
                << buildCalls.load(std::memory_order_relaxed) << ")\n";
      return 1;
    }

    source.writeConstantEffect("0.75");
    std::unique_ptr<onda::plugin::PreparedEngine> changed{
        waitForReplacement(watchReplacements)};
    if (!changed ||
        buildCalls.load(std::memory_order_relaxed) != initialBuildCalls + 1 ||
        !produces(*changed, 0.75F)) {
      std::cerr << "metadata-gated watcher did not rebuild a content edit once "
                << "(initial " << initialBuildCalls << ", current "
                << buildCalls.load(std::memory_order_relaxed) << ", status "
                << worker.status().message << ")\n";
      return 1;
    }
  }

  source.writeConstantEffect("0.5");
  {
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> firstReplacements;
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> firstRetirements;
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> secondReplacements;
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> secondRetirements;
    std::atomic<bool> firstDeactivate{};
    std::atomic<bool> secondDeactivate{};
    std::mutex compileGateMutex;
    std::condition_variable compileGateWake;
    int entered{};
    int concurrent{};
    int maximumConcurrent{};
    bool releaseCompiles{};

    onda::plugin::Worker::BuildFunction countedBuild =
        [&](const std::filesystem::path &path,
            const onda::plugin::Product product, const double sampleRate,
            const int blockSize,
            const std::span<const onda::plugin::BufferFileBinding> bindings,
            const std::optional<onda::plugin::ParameterValues> &parameters) {
          {
            std::unique_lock lock(compileGateMutex);
            ++entered;
            ++concurrent;
            maximumConcurrent = std::max(maximumConcurrent, concurrent);
            compileGateWake.notify_all();
            if (!compileGateWake.wait_for(lock, test::waitTimeout,
                                          [&] { return releaseCompiles; }))
              return onda::plugin::BuildResult{};
          }
          auto result = onda::plugin::PreparedEngine::build(
              path, product, sampleRate, blockSize, bindings, parameters);
          {
            std::lock_guard lock(compileGateMutex);
            --concurrent;
            compileGateWake.notify_all();
          }
          return result;
        };

    onda::plugin::Worker first(onda::plugin::Product::effect, firstReplacements,
                               firstRetirements, firstDeactivate, countedBuild);
    onda::plugin::Worker second(
        onda::plugin::Product::effect, secondReplacements, secondRetirements,
        secondDeactivate, countedBuild);
    first.configure(48'000.0, 8);
    second.configure(48'000.0, 8);
    first.load(source.path(), false);
    second.load(source.path(), false);
    {
      std::unique_lock lock(compileGateMutex);
      if (!compileGateWake.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return entered == 1; })) {
        std::cerr << "parallel worker compilation did not start\n";
        return 1;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    {
      std::lock_guard lock(compileGateMutex);
      if (entered != 1 || maximumConcurrent != 1) {
        std::cerr << "Onda compilers ran concurrently across instances\n";
        return 1;
      }
      releaseCompiles = true;
    }
    compileGateWake.notify_all();
    std::unique_ptr<onda::plugin::PreparedEngine> firstEngine{
        waitForReplacement(firstReplacements)};
    std::unique_ptr<onda::plugin::PreparedEngine> secondEngine{
        waitForReplacement(secondReplacements)};
    if (!firstEngine || !secondEngine || maximumConcurrent != 1) {
      std::cerr << "serialized multi-instance compilation did not complete\n";
      return 1;
    }
  }

  {
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *>
        retirementReplacements;
    onda::plugin::SpscSlot<onda::plugin::PreparedEngine *> retirementQueue;
    std::atomic<bool> retirementDeactivate{};
    {
      std::lock_guard lock(retirementAuditMutex);
      retirementCount = 0;
      retirementThread = {};
    }
    onda::plugin::Worker worker(
        onda::plugin::Product::effect, retirementReplacements, retirementQueue,
        retirementDeactivate, {}, observeRetirement);
    worker.configure(48'000.0, 8);
    worker.load(source.path(), false);
    auto *engine = waitForReplacement(retirementReplacements);
    if (engine == nullptr || !retirementQueue.tryPush(engine)) {
      std::cerr << "could not enqueue an engine retirement\n";
      return 1;
    }
    std::unique_lock lock(retirementAuditMutex);
    if (!retirementAuditWake.wait_for(lock, std::chrono::seconds(5),
                                      [] { return retirementCount > 0; }) ||
        retirementThread == std::this_thread::get_id()) {
      std::cerr << "retired engine was not destroyed by the worker thread\n";
      return 1;
    }
  }

  std::cout << "onda_plugin_tests: passed\n";
  return 0;
}
