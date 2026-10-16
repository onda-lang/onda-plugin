#include "ParameterArrayTests.h"

#include "Engine.h"
#include "TemporaryDirectory.h"
#include "TestNumeric.h"

#include <array>
#include <fstream>
#include <iostream>

bool exerciseParameterArrays() {
  using namespace onda::plugin;
  if constexpr (slotCount < 11)
    return true;
  const auto source = testTemporaryRoot() / "parameter-arrays.onda";
  {
    std::ofstream file(source);
    file << R"(
params {
  gain = 0.25 {0.0, 1.0}
  levels: f64[2] = [0.125, 0.75] {0.0, 1.0}
  large: i64[2] = [9007199254740993, 9007199254740995]
  switches: bool[2] = [true, false]
  solo: f32[1] = [0.375] {0.0, 1.0}
  steps: i32[2] = [3, 7]
  tail: f32[)"
         << slotCount << "] = [";
    for (int index = 0; index < static_cast<int>(slotCount); ++index)
      file << (index == 0 ? "" : ", ") << "0.25";
    file << R"(] {0.0, 1.0}
}
outs { out1, out2 }
sample {
  out1 = f32(gain + levels[0] + levels[1])
  out2 = f32(large[0] - 9007199254740992) + f32(i32(switches[0])) + f32(i32(switches[1])) + f32(steps[1] - steps[0]) + tail[)"
         << slotCount - 11 << "] + tail[" << slotCount - 10 << R"(]
}
)";
  }
  auto built = PreparedEngine::build(source, Product::effect, 48'000.0, 8);
  if (!built.engine) {
    std::cerr << "array program failed: " << built.diagnostic.message << '\n';
    return false;
  }
  const auto mappings = built.engine->parameterMappings();
  if (mappings.size() != slotCount || mappings[0].name != "gain" ||
      mappings[1].name != "levels[0]" || mappings[2].name != "levels[1]" ||
      mappings[7].name != "solo[0]" ||
      mappings[slotCount - 1].name !=
          "tail[" + std::to_string(slotCount - 11) + "]" ||
      mappings[3].defaultPlain != ScalarPlain{std::int64_t{9007199254740993}} ||
      mappings[4].defaultPlain != ScalarPlain{std::int64_t{9007199254740995}}) {
    std::cerr << "parameter arrays did not consume the configured scalar slots "
                 "in declaration order\n";
    return false;
  }
  ParameterState slots;
  for (std::size_t index = 0; index < mappings.size(); ++index)
    slots.setDefault(index, mappings[index].parameterDefault());
  std::array<float, 8> left{}, right{};
  const std::array outputs{left.data(), right.data()};
  const auto renders = [&](float first, float second) {
    if (!built.engine->process(nullptr, outputs.data(), 8, {}, slots))
      return false;
    if (!test::withinTolerance(left[0] - first, 1.0e-6F) ||
        !test::withinTolerance(right[0] - second, 1.0e-6F)) {
      std::cerr << "array elements rendered " << left[0] << ", " << right[0]
                << "; expected " << first << ", " << second << '\n';
      return false;
    }
    return true;
  };
  if (!renders(1.125F, 6.5F))
    return false;
  slots.set(1, 0.5F);
  slots.set(6, 1.0F);
  slots.set(slotCount - 1, 0.75F);
  slots.set(8, 1.0F);
  return renders(1.5F, 10.0F);
}
