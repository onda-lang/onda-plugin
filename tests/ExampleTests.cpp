#include "Engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>

namespace {

using onda::plugin::HostContext;
using onda::plugin::MidiEvent;
using onda::plugin::MidiKind;
using onda::plugin::PreparedEngine;
using onda::plugin::Product;
using onda::plugin::slotCount;

struct Slots {
  std::array<std::atomic<float>, slotCount> values{};
  std::array<std::atomic<float> *, slotCount> pointers{};

  explicit Slots(const PreparedEngine &engine) {
    for (std::size_t index = 0; index < values.size(); ++index)
      pointers[index] = &values[index];
    for (const auto &mapping : engine.parameterMappings())
      values[static_cast<std::size_t>(mapping.parameterIndex)].store(
          static_cast<float>(mapping.defaultNormalized));
  }
};

std::unique_ptr<PreparedEngine> build(std::string_view relativePath,
                                      Product product) {
  const auto path = std::filesystem::path{ONDA_PLUGIN_EXAMPLES_DIR} /
                    std::filesystem::path{relativePath};
  auto result = PreparedEngine::build(path, product, 48'000.0, 64);
  if (!result.engine)
    std::cerr << path << ": " << result.diagnostic.message << '\n';
  return std::move(result.engine);
}

bool finite(std::span<const float> samples) {
  return std::ranges::all_of(samples,
                             [](float value) { return std::isfinite(value); });
}

bool nonSilent(std::span<const float> samples) {
  return std::ranges::any_of(
      samples, [](float value) { return std::abs(value) > 1.0e-7F; });
}

bool exerciseInstrument(std::string_view relativePath,
                        std::span<const MidiEvent> midi,
                        bool expectsPitchBend) {
  auto engine = build(relativePath, Product::instrument);
  if (!engine || !engine->handlesMidi(MidiKind::noteOn) ||
      !engine->handlesMidi(MidiKind::noteOff) ||
      engine->handlesMidi(MidiKind::pitchBend) != expectsPitchBend)
    return false;

  Slots slots{*engine};
  std::array<float, 64> left{};
  std::array<float, 64> right{};
  std::array<float *, 2> outputs{left.data(), right.data()};
  return engine->process(nullptr, outputs.data(), 64, midi, slots.pointers) &&
         finite(left) && finite(right) && nonSilent(left) && nonSilent(right);
}

bool exerciseEffect(std::string_view relativePath, bool expectsTempo) {
  auto engine = build(relativePath, Product::effect);
  if (!engine || engine->handlesHostContext(
                     onda::plugin::HostContextKind::tempo) != expectsTempo)
    return false;

  Slots slots{*engine};
  std::array<float, 64> left{};
  std::array<float, 64> right{};
  left[0] = 0.75F;
  right[0] = -0.5F;
  std::array<float *, 2> inputs{left.data(), right.data()};
  std::array<float *, 2> outputs{left.data(), right.data()};
  HostContext context;
  context.tempo = 93.0;
  return engine->process(inputs.data(), outputs.data(), 64, {}, slots.pointers,
                         context) &&
         finite(left) && finite(right) && nonSilent(left) && nonSilent(right);
}

} // namespace

int main() {
  const std::array polySawMidi{
      MidiEvent{MidiKind::noteOn, 0, 0, 60, 0.8F},
      MidiEvent{MidiKind::pitchBend, 24, 0, 0, 0.75F},
      MidiEvent{MidiKind::noteOff, 48, 0, 60, 0.0F},
  };
  if (!exerciseInstrument("instruments/poly_saw.onda", polySawMidi, true))
    return 1;

  const std::array bellMidi{
      MidiEvent{MidiKind::noteOn, 0, 0, 72, 0.85F},
      MidiEvent{MidiKind::noteOff, 48, 0, 72, 0.0F},
  };
  if (!exerciseInstrument("instruments/fm_bells.onda", bellMidi, false))
    return 1;

  struct Effect {
    std::string_view path;
    bool expectsTempo{};
  };
  constexpr std::array effects{
      Effect{"effects/tempo_ping_pong.onda", true},
      Effect{"effects/reactive_wavefolder.onda"},
      Effect{"effects/transient_sculptor.onda"},
      Effect{"effects/orbit_flanger.onda"},
  };
  for (const auto &effect : effects)
    if (!exerciseEffect(effect.path, effect.expectsTempo))
      return 1;

  std::cout << "onda_example_tests: passed\n";
}
