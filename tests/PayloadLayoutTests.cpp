#include "PayloadLayoutTests.h"

#include "Engine.h"
#include "JucePath.h"
#include "RealtimeAudit.h"
#include "TemporaryDirectory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>

namespace {

bool negativeZeroPayload() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "negative-zero-payload.onda")};
  if (!source.replaceWithText(R"(
struct Zeros:
  single: f32
  wide: f64
event send(single: f32, wide: f64, fixed: Zeros[1], dynamic: Zeros[]) {}
sample { out1 = 0.0 }
)"))
    return false;
  const auto built = PreparedEngine::build(
      pathFromJuce(source.getFullPathName()), Product::effect, 48'000.0, 8);
  if (!built.engine) {
    std::cerr << built.diagnostic.message << '\n';
    return false;
  }
  const auto &event = built.engine->eventMappings()[0];
  const auto values = juce::JSON::parse(R"([
    "-0", "-0", [{"single":0.0,"wide":"-0"}],
    [{"single":"-0","wide":-0.0}]
  ])");
  std::array<std::byte, 64> payload{};
  std::array<std::uint8_t, 40> expected{};
  expected[24] = 1; // Dynamic slice length.
  for (const auto signByte : {3U, 11U, 23U, 31U, 39U})
    expected[signByte] = 0x80;
  std::size_t written{};
  std::string error;
  if (!encodePayload(event.schema, event.payloadPlan.get(), values, payload,
                     written, error) ||
      written != expected.size() ||
      std::memcmp(payload.data(), expected.data(), expected.size()) != 0) {
    std::cerr << "Event zero signs were not preserved: " << error << '\n';
    return false;
  }
  return true;
}

bool nestedDelegateLayout() {
  using namespace onda::plugin;
  const juce::File source{
      pathToJuce(testTemporaryRoot() / "nested-delegate-layout.onda")};
  if (!source.replaceWithText(R"(
struct Item:
  flag: bool
  value: i64
struct Group:
  items: Item[2]
  pair: (i32, f64)
delegate reported(prefix: bool, groups: Group[], fixed: Item[2], empty: Item[], tail: i64)
event send(prefix: bool, groups: Group[], fixed: Item[2], empty: Item[], tail: i64):
  reported(prefix, groups, fixed, empty, tail)
sample { out1 = 0.0 }
)"))
    return false;
  auto built = PreparedEngine::build(pathFromJuce(source.getFullPathName()),
                                     Product::effect, 48'000.0, 8);
  if (!built.engine) {
    std::cerr << built.diagnostic.message << '\n';
    return false;
  }
  const auto &event = built.engine->eventMappings()[0];
  const auto values = juce::JSON::parse(R"([
    true,
    [{"items":[{"flag":true,"value":"1"},{"flag":false,"value":"2"}],"pair":[10,0.5]},
     {"items":[{"flag":false,"value":"3"},{"flag":true,"value":"9223372036854775807"}],"pair":[20,-2.0]}],
    [{"flag":false,"value":"-1"},{"flag":true,"value":"-2"}],
    [], "-9223372036854775808"
  ])");
  std::array<std::byte, 256> payload{};
  std::size_t written{};
  std::string error;
  if (!encodePayload(event.schema, event.payloadPlan.get(), values, payload,
                     written, error)) {
    std::cerr << error << '\n';
    return false;
  }
  RuntimeLogSink sink;
  built.engine->attachLogSink(sink);
  ParameterState parameters;
  EventTriggerResult result{};
  if (!test::auditRealtime([&] {
        result = built.engine->triggerEvent(
            event.index, std::span{payload}.first(written), parameters);
      }) ||
      result != EventTriggerResult::success)
    return false;
  RuntimeLogEntry entry;
  constexpr std::string_view expected =
      "delegate reported: prefix=true groups.items.flag=[true, false, false, "
      "true] "
      "groups.items.value=[1, 2, 3, 9223372036854775807] "
      "groups.pair.__0=[10, 20] groups.pair.__1=[0.5, -2] "
      "fixed.flag=[false, true] fixed.value=[-1, -2] "
      "empty.flag=[] empty.value=[] tail=-9223372036854775808";
  if (!sink.tryPop(entry) ||
      std::string_view{entry.text.data(), entry.textBytes} != expected) {
    std::cerr << "Nested delegate layout was not decoded correctly\n";
    return false;
  }
  return true;
}

} // namespace

bool exerciseEventPayloadLayout() {
  using namespace onda::plugin;
  EventMapping event;
  {
    const juce::File source{
        pathToJuce(testTemporaryRoot() / "event-payload-layout.onda")};
    if (!source.replaceWithText(
            "event send(head: bool, values: f32[], tail: i64) {}\n"
            "sample { out1 = 0.0 }\n"))
      return false;
    const auto built = PreparedEngine::build(
        pathFromJuce(source.getFullPathName()), Product::effect, 48'000.0, 8);
    if (!built.engine || built.engine->eventMappings().size() != 1U) {
      std::cerr << built.diagnostic.message << '\n';
      return false;
    }
    event = built.engine->eventMappings()[0];
  }
  // The engine and compilation are gone. A published metadata snapshot must
  // keep the program-owned layout alive until its last consumer releases it.
  std::weak_ptr<const onda_payload_plan_t> owner = event.payloadPlan;
  if (owner.expired())
    return false;
  std::array<std::byte, 64> payload{};
  std::size_t written{};
  std::string error;
  const juce::Array<juce::var> values{
      true, juce::var{juce::Array<juce::var>{1.5, -2.25}}, "9007199254740993"};
  constexpr std::array<std::uint8_t, 21> expected{
      1,    2,    0, 0, 0, 0, 0, 0xc0, 0x3f, 0, 0,
      0x10, 0xc0, 1, 0, 0, 0, 0, 0,    0x20, 0};
  if (!encodePayload(event.schema, event.payloadPlan.get(), values, payload,
                     written, error) ||
      written != expected.size() ||
      std::memcmp(payload.data(), expected.data(), expected.size()) != 0) {
    std::cerr << "Resolved event layout encoded incorrect bytes: " << error
              << '\n';
    return false;
  }
  const juce::Array<juce::var> empty{false, juce::var{juce::Array<juce::var>{}},
                                     "-9223372036854775808"};
  constexpr std::array<std::uint8_t, 13> expectedEmpty{0, 0, 0, 0, 0, 0,   0,
                                                       0, 0, 0, 0, 0, 0x80};
  if (!encodePayload(event.schema, event.payloadPlan.get(), empty, payload,
                     written, error) ||
      written != expectedEmpty.size() ||
      std::memcmp(payload.data(), expectedEmpty.data(), expectedEmpty.size()) !=
          0)
    return false;
  payload.fill(std::byte{0xa5});
  if (encodePayload(event.schema, event.payloadPlan.get(), empty,
                    std::span{payload}.first(12), written, error) ||
      written != 0U || !std::ranges::all_of(payload, [](const auto byte) {
        return byte == std::byte{0xa5};
      }))
    return false;
  event = {};
  return owner.expired() && negativeZeroPayload() && nestedDelegateLayout();
}
