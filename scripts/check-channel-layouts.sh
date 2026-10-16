#!/usr/bin/env bash
set -euo pipefail

# Reuse the default Linux build's pinned SDK and JUCE sources. These custom
# builds validate supported configurations without delaying default releases.
source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
default_build="$(cd "${1:?usage: check-channel-layouts.sh default-build}" && pwd)"
onda_version="$(tr -d '\r\n' < "$source_root/onda-version")"

# Explicit local SDK/source overrides live in the cache; automatically fetched
# dependencies use the default build's _deps directory.
cache_value() {
  local entry
  while IFS= read -r entry; do
    if [[ "$entry" == "$1":*'='* ]]; then
      printf '%s\n' "${entry#*=}"
      return
    fi
  done < "$default_build/CMakeCache.txt"
}
sdk_archive="$(cache_value ONDA_SDK_ARCHIVE)"
sdk_checksums="$(cache_value ONDA_SDK_CHECKSUMS)"
release_source="$(cache_value ONDA_RELEASE_SOURCE_DIR)"
juce_root="${ONDA_JUCE_ROOT:-$(cache_value ONDA_JUCE_ROOT)}"
validator="${ONDA_VST3_VALIDATOR:-$(cache_value ONDA_VST3_VALIDATOR)}"

for layout in '2 16 2 1' '0 16 2 32' '1 0 1 32' '4 4 4 64'; do
  read -r inputs controls outputs slots <<< "$layout"
  layout_build="$source_root/build-layout-$inputs-$controls-$outputs-$slots"
  cmake -S "$source_root" -B "$layout_build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DONDA_PLUGIN_INPUT_CHANNELS="$inputs" \
    -DONDA_PLUGIN_CONTROL_INPUTS="$controls" \
    -DONDA_PLUGIN_OUTPUT_CHANNELS="$outputs" \
    -DONDA_PLUGIN_PARAMETER_SLOTS="$slots" \
    -DONDA_PLUGIN_BUILD_BROWSER_TESTS=OFF \
    -DONDA_PLUGIN_BUILD_VST3_AUDIT=OFF \
    -DONDA_VST3_VALIDATOR="$validator" \
    -DONDA_SDK_ARCHIVE="${sdk_archive:-$default_build/_deps/onda-$onda_version-linux-x64.tar.xz}" \
    -DONDA_SDK_CHECKSUMS="${sdk_checksums:-$default_build/_deps/$onda_version-SHA256SUMS.txt}" \
    -DONDA_RELEASE_SOURCE_DIR="${release_source:-$default_build/_deps/ondareleasesource-src}" \
    -DONDA_JUCE_ROOT="${juce_root:-$default_build/_deps/juce-src}"
  cmake --build "$layout_build" --config Release \
    --parallel 2
  ctest --test-dir "$layout_build" -C Release --output-on-failure --no-tests=error -L '^core$'
done
