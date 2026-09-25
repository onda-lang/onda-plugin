<h1>
  <img src="assets/svg/onda-logo-dark-circle.svg" alt="onda logo" width="40" align="absmiddle" /> Onda VST3 Plugin
</h1>

`OndaSynth` and `OndaFX` are two VST3 plugins that allow you to run the 
the [Onda](https://onda-lang.org) audio programming language in your favorite DAW.

[Pre-built binaries](https://github.com/onda-lang/onda-plugin/releases) are available for Windows, macOS and Linux.
The pre-built macOS VST3 binaries are not signed.

## Examples

The [`examples`](examples) contain tested, plug-in-ready instruments and effects
that show basic Onda programs. Load instrument patches in `OndaSynth` and effects in `OndaFX`
through the embedded run view. 

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
audio processing at their offsets. 

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

While transport is playing, sample and second positions are projected from the
host callback start to the logical block boundary. Quarter-note position is
projected at an interior boundary only when the same snapshot also supplies
tempo. Stopped timeline positions remain unchanged, including when the program
does not declare a `transport` event. Bar position and loop points use the host-provided values. 

## Parameters and automation

The VST3 exposes 32 permanent, normalized automation slots so parameter IDs
and saved DAW automation remain stable while Onda programs are recompiled.
After a successful compile, mapped slots present their Onda names, units,
defaults, boolean/discrete step counts, and normalized-to-plain text conversion
to the host. Logarithmic scaling, curves, and step snapping use Onda's processor
ABI. 

## Audio buffers

An `.ondaproject` input uses the immutable buffer defaults retained by Onda's
compiled program. External buffers can also be bound by name to an audio file
from the embedded run view, either for a source input or as an explicit project
override. The picker accepts every format registered by this JUCE build; the
cross-platform defaults include WAV, AIFF, FLAC, and Ogg Vorbis. Files are
decoded on the background worker into pointer-stable, interleaved `f32` storage
before the replacement engine is published. In addition to the JUCE formats,
the loader accepts Onda's canonical `.ondabuffer` container.

Audio-file bindings must be read-only: programs that may write a bound buffer
are rejected during preparation because Onda project assets are immutable.
The worker uses native filesystem notifications for source and bound audio
files, so an edit prepares a new engine while the current one keeps running.

Plugin state stores buffer names and absolute audio-file paths for live disk
reloading, while the project image always contains the matching canonical
assets for deterministic DAW restore. Moving a linked audio file therefore
falls back to the saved project image. Clearing a binding restores an available
`.ondaproject` default; otherwise the current engine remains inactive until
every declared buffer is bound. Failed replacements retain the last complete project checkpoint.
If a replacement requires new buffer bindings, the view exposes those choices
while the previous engine keeps playing. Binding or clearing these pending
buffers preserves the last complete checkpoint until preparation succeeds.

## Build

Requirements:

- CMake 3.22+, Ninja, and a C++20 compiler
- The plug-in version selected by [`plugin-version`](plugin-version)
- The Onda release SDK selected by [`onda-version`](onda-version)
- JUCE selected by [`juce-version`](juce-version)
- Linux editor builds: GTK 3 and WebKitGTK 4.1 development packages

Configure and build the plugins:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To run tests:

```sh
ctest --test-dir build --output-on-failure
```

The VST3 audio layout is fixed at compile time and defaults to 2 inputs and 2
outputs. Custom builds can expose between 0 and 64 inputs and between 1 and 64
outputs; for example:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
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
