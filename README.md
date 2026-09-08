# Onda VST3 plugin

This repository builds two VST3 plugins: `OndaSynth.vst3` and `OndaFX.vst3`.
Release builds expose 2 audio inputs and 2 audio outputs. `OndaSynth` clears
its outputs while inactive; `OndaFX` passes through corresponding input
channels and clears any additional outputs.

## Examples

[`examples`](examples) contains tested, plug-in-ready instruments and effects
that exercise MIDI, host tempo, polyphony, dynamics, nonlinear processing, and
modulated delay. Load instrument patches in `OndaSynth` and effects in `OndaFX`
through the embedded run view. The complete collection is included in every
release archive.

The embedded run view also shows a live scope of the plug-in output while its
editor is open. Programs declaring canonical note events also get a read-only
MIDI keyboard: host note-on/off messages illuminate its keys, while MIDI device
selection and note input remain owned by the DAW. Editor dimensions and the
Sliders/Knobs choice are retained when the view is reopened and saved in the DAW
project. Event argument edits survive parameter automation and log updates;
loading a new program or explicitly resetting the arguments restores defaults.

Host preparation finishes the requested specialization before playback begins.
Restores and rebuilds initialize ordinary and pinned state from a snapshot of
the current host parameters. Selecting a new patch uses its declared defaults.
Offline rendering also waits for state restored after preparation, without
requiring the editor or its message loop. Loaded programs report an infinite
tail to the host because arbitrary Onda DSP can sustain indefinitely; configure
the desired render-tail duration in the DAW.

## DSP events

The plugin recognizes this canonical MIDI surface:

```onda
event note_on(id: i32, channel: i32, key: i32, velocity: f32)
event note_off(id: i32, channel: i32, key: i32, velocity: f32)
event poly_pressure(channel: i32, key: i32, pressure: f32)
event pitch_bend(channel: i32, value: f32)
event channel_pressure(channel: i32, pressure: f32)
event cc(channel: i32, index: i32, value: f32)
event program_change(channel: i32, program: i32)
```

Instruments via `OndaSynth` require exact `note_on` and `note_off` declarations; 
every other declaration is optional. Effects may omit all of them. Declaring a
name with a different payload rejects the load. Channels are zero-based,
ordinary MIDI notes use `id = -1`, and all continuous values except program
number are normalized to `[0, 1]`. Pitch bend uses `0.5` as its center.
Incoming messages whose event is not declared are ignored without splitting
audio processing at their offsets. Unsupported system/SysEx messages are ignored
without allocating in the audio callback.

JUCE playhead data is exposed through these optional, read-only host-context
events:

```onda
event transport(playing: bool, recording: bool, looping: bool)
event sample_position(sample: i64)
event time_position(seconds: f64)
event tempo(bpm: f64)
event musical_position(quarter_note: f64)
event bar_position(start_quarter_note: f64)
event time_signature(numerator: i32, denominator: i32)
event loop_region(start_quarter_note: f64, end_quarter_note: f64)
event render_mode(realtime: bool)
```

At each logical Onda block start, the callback snapshots host parameters and
then dispatches the declared context events in the order above before MIDI and
audio processing. When declared, `render_mode` is dispatched unconditionally.
Every other event is dispatched only when the current JUCE `PositionInfo`
supplies its field; there are no separate availability flags. If a host stops
supplying a field, its handler is not called again, so the Onda program decides
whether and how long to retain the previous value.

The prepared engine retains an allocation-free presence table built from Onda
event metadata. Undeclared context fields are not read, projected, packed, or
dispatched. If the program declares no position-based context event, the audio
callback does not request JUCE playhead position data at all. A declared
`musical_position` also reads tempo when available because tempo is required to
project PPQ to an interior logical-block boundary.

While transport is playing, sample and second positions are projected from the
host callback start to the logical block boundary. Quarter-note position is
projected at an interior boundary only when the same snapshot also supplies
tempo. Stopped timeline positions remain unchanged, including when the program
does not declare a `transport` event. Bar position and
loop points use the host-provided values. Host context is not stored in plugin
state and cannot be used to control the DAW.


## Audio-file buffers

An `.ondaproject` input uses the immutable buffer defaults retained by Onda's
compiled program. External buffers can also be bound by name to an audio file
from the embedded run view, either for a source input or as an explicit project
override. The picker accepts every format registered by this JUCE build; the
cross-platform defaults include WAV, AIFF, FLAC, and Ogg Vorbis. Files are
decoded on the background worker into pointer-stable, interleaved `f32` storage
before the replacement engine is published. In addition to the JUCE formats,
the loader accepts Onda's canonical `.ondabuffer` container.

The decoded channel count must match a mono or fixed-channel Onda buffer;
dynamic-channel buffers accept the file's channel count. Samples are not
resampled: the file's original sample rate is supplied to Onda with the buffer.
Audio-file bindings must be read-only: programs that may write a bound buffer
are rejected during preparation because Onda project assets are immutable.
The worker watches both source and bound audio files, so an edited audio file
prepares a new engine while the current one keeps running.

Plugin state stores buffer names and absolute audio-file paths for live disk
reloading, while the project image always contains the matching canonical
assets for deterministic DAW restore. Moving a linked audio file therefore
falls back to the saved project image. Clearing a binding restores an available
`.ondaproject` default; otherwise the current engine remains inactive until
every declared buffer is bound.
The source link and remaining bindings are saved even in this incomplete state.
An initial source selection and partially bound buffers are saved immediately,
including before the host prepares playback. Failed replacements retain the
last complete project checkpoint.
If a replacement requires new buffer bindings, the view exposes those choices
while the previous engine keeps playing. Binding or clearing these pending
buffers preserves the last complete checkpoint until preparation succeeds.

Runtime output retained for the editor is bounded to 1,024 records and 256 KiB
of text and source metadata. Older records are discarded first and included in
the corresponding drop count shown by the logger.

## Build

Requirements:

- CMake 3.22+, Ninja, and a C++20 compiler
- The plug-in version selected by [`plugin-version`](plugin-version)
- The Onda release SDK selected by [`onda-version`](onda-version)
- JUCE 8.0.13 at
  `7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2`
- Linux editor builds: GTK 3 and WebKitGTK 4.1 development packages

Configure and build the plugins:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The VST3 audio layout is fixed at compile time and defaults to 2 inputs and 2
outputs. Custom builds can expose between 0 and 64 inputs and between 1 and 64
outputs; for example:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DONDA_PLUGIN_INPUT_CHANNELS=4 \
  -DONDA_PLUGIN_OUTPUT_CHANNELS=8
```

An Onda program may declare up to the configured number of channels. Unused
host inputs are ignored and unused host outputs are cleared. The published
archives retain the default 2-input/2-output layout.

CMake reads `plugin-version` for the embedded VST3 version and package release
identity. `-DONDA_PLUGIN_VERSION=x.y.z` overrides it explicitly; otherwise the
`ONDA_PLUGIN_VERSION` environment variable takes precedence over the file.

Independently, `onda-version` selects the Onda SDK and the exact Onda source
revision used for embedded run-view resources. `-DONDA_VERSION=x.y.z`
overrides that pin; otherwise the `ONDA_VERSION` environment variable takes
precedence over the file. This separation allows plug-in-only fixes to ship
without changing the Onda SDK dependency.

The current pin uses the official
[Onda 0.8.3 release](https://github.com/onda-lang/onda/releases/tag/0.8.3).
The default build downloads its SDK, verifies it against the release's
`SHA256SUMS.txt`, and embeds run-view resources from the same release tag.

For a private Onda release, download the archive and its checksums using
authenticated GitHub CLI and pass them explicitly:

```sh
onda_version="$(tr -d '\r\n' < onda-version)"
gh release download "$onda_version" --repo onda-lang/onda \
  --pattern "onda-$onda_version-linux-x64.tar.xz" \
  --pattern SHA256SUMS.txt
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DONDA_SDK_ARCHIVE="$PWD/onda-$onda_version-linux-x64.tar.xz" \
  -DONDA_SDK_CHECKSUMS="$PWD/SHA256SUMS.txt"
```

`ONDA_JUCE_ROOT` remains available as an optional exact-revision JUCE checkout;
without it, CMake fetches the pinned JUCE revision. The bundles are written to
`OndaSynth_artefacts/Release/VST3` and `OndaFX_artefacts/Release/VST3`. They are
never copied into a system or user plugin directory.

For release verification, point CMake at Steinberg's VST3 validator so both
bundles are included in the normal test suite:

```sh
cmake -S . -B build \
  -DONDA_VST3_VALIDATOR=/path/to/validator
cmake --build build
ctest --test-dir build --output-on-failure
```

The same checks can be run directly with
`scripts/validate-vst3.sh /path/to/validator build`.

## Releases

Pull requests and pushes to `main` run clean Release builds and tests for Linux
x64, Windows x64, and macOS arm64, including Steinberg's VST3 validator for both
bundles. Linux also validates a zero-input build. Pushing a semantic version tag
matching `plugin-version`, with or without a leading `v`, runs the same matrix and
publishes the platform archives:

```sh
version="$(tr -d '\r\n' < plugin-version)"
git tag "$version"
git push origin "$version"
```

Each archive contains `OndaSynth.vst3`, `OndaFX.vst3`, the tested example
collection, the project license, and third-party notices. The workflow creates
a checksum manifest and publishes a new, immutable GitHub release. It never
replaces assets on an existing release.
Manual runs build without publishing by default; publishing can be enabled only
when the workflow is dispatched from the matching release tag.

macOS archives are unsigned and unnotarized. Users may need to approve the
plug-ins explicitly in macOS Privacy & Security before a host can load them.
