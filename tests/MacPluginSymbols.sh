#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -eq 0 ]]; then
    echo "Usage: MacPluginSymbols.sh <VST3 module>..." >&2
    exit 2
fi

for module in "$@"; do
    symbols="$(nm -gUj "$module")"
    for entry in _GetPluginFactory _bundleEntry _bundleExit; do
        if ! printf '%s\n' "$symbols" | awk -v entry="$entry" \
            '$0 == entry { found = 1 } END { exit !found }'; then
            echo "$module is missing VST3 entry point $entry" >&2
            exit 1
        fi
    done

    leaked="$(printf '%s\n' "$symbols" | c++filt | awk \
        '/juce::/ { count++; if (count <= 10) print } END { if (count) print count " exported JUCE symbols" }')"
    if [[ -n "$leaked" ]]; then
        printf '%s exports private JUCE symbols:\n%s\n' "$module" "$leaked" >&2
        exit 1
    fi
    echo "$module: VST3 entry points exported; JUCE symbols private"
done
