<h1>
  <img src="assets/svg/onda-logo-dark-circle.svg" alt="onda logo" width="40" align="absmiddle" /> Onda VST3 Plugin
</h1>

`OndaSynth` and `OndaFX` are VST3 plugins for running the
[Onda](https://onda-lang.org) audio programming language in your DAW.

[Pre-built binaries](https://github.com/onda-lang/onda-plugin/releases) are available for Windows, macOS and Linux.
Release builds target Linux x64 with a Debian 12 baseline (glibc 2.36), Windows
x64 with the Visual Studio 2022 toolchain and static MSVC runtime, and macOS
arm64 with a minimum deployment target of macOS 14.0.

Release signing status:

- **macOS:** binaries are not signed with an Apple Developer ID and are not notarized. macOS may
  block downloaded plug-ins until you approve them in **System Settings → Privacy & Security**.
  See [Apple's instructions](https://support.apple.com/102445).
- **Windows:** plug-in binaries are not code-signed (Authenticode).

## Examples

Load instrument patches in `OndaSynth` and effects in `OndaFX` through the
embedded editor. The [examples](examples) include ready-to-use patches for both.

## DSP events

Onda programs can receive these MIDI events:

```onda
event note_on(id: i32, channel: i32, key: i32, velocity: f32)
event note_off(id: i32, channel: i32, key: i32, velocity: f32)
event poly_pressure(channel: i32, key: i32, pressure: f32)
event pitch_bend(channel: i32, value: f32)
event channel_pressure(channel: i32, pressure: f32)
event cc(channel: i32, index: i32, value: f32)
event program_change(channel: i32, program: i32)
```

`OndaSynth` requires `note_on` and `note_off` with the signatures above; all other
events are optional. `OndaFX` may omit them all. Channels are zero-based, MIDI
notes use `id = -1`, and velocity, pressure, bend, and CC values use `[0, 1]`.
Pitch bend is centered at `0.5`.

Each callback accepts up to 4,096 MIDI events and MIDI-controller automation
points combined. If that limit is exceeded, the plugin discards that MIDI batch
and releases held host notes. Ordinary parameter and control automation continues.

These optional events provide DAW transport and timing information:

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

Timing events run at the start of each Onda block, before MIDI and audio.
These events are only sent when the DAW supplies their value.

## Parameters and automation

The plugin provides 32 DAW automation slots for `params` by default. Mapped slots use the
loaded program's parameter names and units. Array elements each consume one
slot, in declaration order, up to the configured limit. Slots remain stable when the
program is recompiled.

Scalar numeric parameters without a declared range appear in **Params** and map to
DAW slots using a plugin-owned 0–1 control range. Declare an explicit range when
controls should cover other values, for example `frequency = 440.0 {20.0, 20000.0}`.
Mapped parameter defaults must be finite. An unsupported default rejects the
replacement and keeps the previous working program and automation mapping.

Continuous `params` automation follows the DAW's normalized linear curve,
sampled at the start of each Onda block. Discrete parameters hold the latest
point at that boundary.

**Block Size** controls how often params automation updates.
Sample rate comes from the DAW.

The **Smooth** value only affects changes made in the UI editor.
DAW automation bypasses parameter smoothing.

## Audio inputs and outputs

The default layout has two main audio inputs and two outputs. Declare channels
as individual `f32` ports or `f32[N]` arrays, where `N` is the channel count.

## Control inputs

The default build provides 16 optional mono control inputs. In the stereo
layout, declare the two main input channels first; subsequent `ins` map to
`Control 1`, `Control 2`, and so on. See
[external_control.onda](examples/effects/external_control.onda) for an example.

Each control appears as a named, automatable DAW parameter. Unlike `params`, control input automation is sample-accurate.
To supply an external signal, enable and route the corresponding bus through your DAW’s sidechain or auxiliary inputs.

## Audio buffers

Drop a single supported audio or `.ondabuffer` file onto a buffer card to bind
or replace its contents. Supported formats include WAV,
AIFF, FLAC, and Ogg Vorbis through JUCE's audio decoders, producing `f32` samples.
Typed `.ondabuffer` assets preserve `bool`, `i32`, `i64`, `f32`, or `f64` elements.
File bindings must be read-only and match the declaration's
element type and channel count. Project assets provide immutable defaults.

Your DAW project saves the program, buffer data, and plugin settings. If linked
files are unavailable when reopening it, the plugin restores the saved version.

## Build

Requires CMake 3.24+, Ninja, and a C++20 compiler. Linux also needs X11,
Fontconfig, FreeType, and ALSA development packages and a Vulkan driver.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The build uses the SDK pinned in `onda-version` and JUCE revision in `juce-version`.
Each VST3 bundle includes its `onda_ui` library and font notices under
`Contents/Resources/onda_ui`, so it can be moved as a complete bundle.
On Windows the renderer is named `onda_ui.bin` to keep DAWs from scanning it as a plugin DLL.

For development against a local Onda checkout, first build its libraries there:

```sh
cargo build --release -p onda_api -p onda_ui --features onda_ui/ffi
```

Then configure this repository with that checkout:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DONDA_DEVELOPMENT_SOURCE_DIR=/path/to/onda
cmake --build build
```

`ONDA_GPU_UI_LIBRARY` optionally overrides the UI library path at build or run time.
An empty build option follows the selected SDK on every configure. Use a
library/header from the same Onda build when overriding it.

Custom layouts use `ONDA_PLUGIN_INPUT_CHANNELS`, `ONDA_PLUGIN_CONTROL_INPUTS`,
`ONDA_PLUGIN_OUTPUT_CHANNELS`, and `ONDA_PLUGIN_PARAMETER_SLOTS`. Main and control
inputs together must not exceed 64; outputs and parameter slots support 1–64.
Set control inputs to zero to omit those buses.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DONDA_PLUGIN_INPUT_CHANNELS=4 -DONDA_PLUGIN_CONTROL_INPUTS=8 \
  -DONDA_PLUGIN_OUTPUT_CHANNELS=8 -DONDA_PLUGIN_PARAMETER_SLOTS=64
```

Run the release test groups on a desktop (or under `xvfb-run -a` on Linux):

```sh
ctest --test-dir build -C Release --output-on-failure -L '^(core|native)$'
```

`plugin-version` controls the plugin release version.
See [architecture](docs/architecture.md) for implementation invariants and
[test contracts](tests/README.md) for CI selections, native editor checks, and
DAW validation.
