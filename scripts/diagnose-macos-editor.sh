#!/usr/bin/env bash
# Run with the diagnostic VST3 build from codex/macos14-editor-startup.
set -euo pipefail

step() {
  printf '\n>>> %s\n' "$1"
  read -r -p '    [Enter when done] ' _
}

capture() {
  local variable="$1" question="$2" answer
  printf '\n>>> %s\n' "$question"
  read -r -p '    > ' answer
  printf -v "$variable" '%s' "$answer"
}

if [[ "$(uname -s)" != Darwin ]]; then
  echo 'Run this diagnostic on the Mac where the editor is blank.' >&2
  exit 1
fi

reaper_app="${1:-/Applications/REAPER.app}"
reaper_binary="$reaper_app/Contents/MacOS/REAPER"
if [[ ! -x "$reaper_binary" ]]; then
  echo "REAPER not found: $reaper_binary" >&2
  echo 'Pass the path to your REAPER.app as the first argument.' >&2
  exit 1
fi

report_directory="$(mktemp -d "${TMPDIR:-/tmp}/onda-editor-startup.XXXXXX")"
report_log="$report_directory/reaper.log"
step 'Install the diagnostic OndaSynth.vst3 from the native-editor artifact in your usual VST3 folder, then quit every REAPER instance.'

printf '\nStarting REAPER with Onda startup tracing. Log: %s\n' "$report_log"
ONDA_EDITOR_DIAGNOSTICS=1 "$reaper_binary" -newinst -new \
  >"$report_directory/stdout.log" 2>"$report_log" &
reaper_process=$!
step 'In the new project, add a track and OndaSynth, open its editor, and leave it open for at least 10 seconds.'
capture UI_RESULT 'Does the editor show its controls, only the background, or an error?'
step 'Quit this REAPER instance and discard the temporary project.'
if wait "$reaper_process"; then
  reaper_exit_code=0
else
  reaper_exit_code=$?
fi

printf '\n--- Captured ---\n'
printf 'MACOS=%s\nUI_RESULT=%s\nREAPER_EXIT=%s\n' \
  "$(sw_vers -productVersion)" "$UI_RESULT" "$reaper_exit_code"
if ! grep -F '[DEBUG-onda-editor]' "$report_log"; then
  echo 'No Onda trace was captured. Check that the diagnostic plugin replaced the release build.'
fi
printf '\nFull log retained at %s\n' "$report_log"
