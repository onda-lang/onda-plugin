#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
scratch_dir="$(mktemp -d)"
trap 'rm -rf "$scratch_dir"' EXIT
mkdir -p "$scratch_dir/bin" "$scratch_dir/default build"

# Exercise the real layout driver without building four copies of the plugin.
cat > "$scratch_dir/bin/cmake" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ "$1" == --build ]]; then exit 0; fi
source_found=false
revision_found=false
for argument in "$@"; do
  case "$argument" in
    -DONDA_RELEASE_SOURCE_DIR=*)
      [[ "${argument#*=}" == "$ONDA_LAYOUT_TEST_SOURCE" ]]
      source_found=true
      ;;
    -DONDA_RELEASE_SOURCE_REVISION=*)
      [[ "${argument#*=}" == "$ONDA_LAYOUT_TEST_REVISION" ]]
      revision_found=true
      ;;
  esac
done
if [[ "$source_found" != true || "$revision_found" != true ]]; then
  echo 'Layout configuration lost the SDK resource checkout or revision' >&2
  exit 1
fi
printf 'configured\n' >> "$ONDA_LAYOUT_TEST_LOG"
SH
printf '#!/usr/bin/env bash\nexit 0\n' > "$scratch_dir/bin/ctest"
chmod +x "$scratch_dir/bin/cmake" "$scratch_dir/bin/ctest"
export PATH="$scratch_dir/bin:$PATH"
export ONDA_LAYOUT_TEST_SOURCE="$scratch_dir/resource checkout"
export ONDA_LAYOUT_TEST_LOG="$scratch_dir/configurations"

for revision in '' 0123456789abcdef0123456789abcdef01234567; do
  export ONDA_LAYOUT_TEST_REVISION="$revision"
  cat > "$scratch_dir/default build/CMakeCache.txt" <<CACHE
ONDA_SDK_ARCHIVE:FILEPATH=$scratch_dir/sdk.tar.xz
ONDA_SDK_CHECKSUMS:FILEPATH=$scratch_dir/SHA256SUMS.txt
ONDA_RELEASE_SOURCE_DIR:PATH=$ONDA_LAYOUT_TEST_SOURCE
ONDA_RELEASE_SOURCE_REVISION:STRING=$revision
ONDA_JUCE_ROOT:PATH=$scratch_dir/juce
CACHE
  : > "$ONDA_LAYOUT_TEST_LOG"
  bash "$source_root/scripts/check-channel-layouts.sh" "$scratch_dir/default build"
  [[ "$(wc -l < "$ONDA_LAYOUT_TEST_LOG")" -eq 4 ]]
done
