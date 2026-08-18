#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 /path/to/vst3-validator [build-directory]" >&2
  exit 2
fi

validator=$1
root=${2:-build}

"$validator" \
  -q \
  "$root/OndaSynth_artefacts/Release/VST3/OndaSynth.vst3"
"$validator" \
  -q \
  "$root/OndaFX_artefacts/Release/VST3/OndaFX.vst3"
