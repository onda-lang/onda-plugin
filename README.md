<h1>
  <img src="assets/svg/onda-logo-dark-circle.svg" alt="onda logo" width="40" align="absmiddle" /> Onda VST3 Plugin
</h1>

`OndaSynth` and `OndaFX` are VST3 plugins for running the
[Onda](https://onda-lang.org) audio programming language in your DAW.

[Pre-built binaries](https://github.com/onda-lang/onda-plugin/releases) are available for Windows, macOS and Linux.
The macOS VST3 binaries require macOS 14 (Sonoma) or later and are not signed.

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
Except for `render_mode`, an event is sent only when the DAW supplies its value.
Initialize any fallback values your program needs, such as a default tempo.

## Parameters and automation

The plugin provides 32 DAW automation slots for `params` by default. Mapped slots use the
loaded program's parameter names and units. Array elements each consume one
slot, in declaration order, up to the configured limit. Slots remain stable when the
program is recompiled.

Scalar numeric parameters without a declared range appear in **Params** and map to
DAW slots using a plugin-owned 0–1 control range. Declare an explicit range when
controls should cover other values, for example `frequency = 440.0 {20.0, 20000.0}`.

Continuous `params` automation follows the DAW's normalized linear curve,
sampled at the start of each Onda block. Discrete parameters hold the latest
point at that boundary. 

**Block Size** controls how often params automation updates.

The **Smooth** value only affects changes made in the UI editor. 
DAW automation normally bypasses smoothing. 

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
The routed signal takes precedence over the parameter value.

Controls start at their declared input default, or zero. Declare an input range
to set the values covered by automation, which maps linearly to that range.

## Audio buffers

Drop a single supported audio or `.ondabuffer` file onto a buffer card to bind
or replace its contents. The target card highlights during the drag. **Bind** and
**Replace** also open a file picker. Supported formats include WAV,
AIFF, FLAC, and Ogg Vorbis through JUCE's audio decoders, producing `f32` samples.
Typed `.ondabuffer` assets preserve `bool`, `i32`, `i64`, `f32`, or `f64` elements,
including tables. File bindings must be read-only and match the declaration's
element type and channel count. Project assets provide immutable defaults.
**Clear** unbinds the slot, including a project default, and saves that choice.
Unbound buffers return neutral values while parameters and controls remain usable.

Binding or clearing a buffer rebuilds the runtime instance using the existing
compiled program. Linked source and buffer files reload automatically after
edits. Failed reloads keep the previous working program playing.

Your DAW project saves the program, buffer data, and plugin settings. If linked
files are unavailable when reopening it, the plugin restores the saved version.

## Build

Requirements:

- CMake 3.22+, Ninja, and a C++20 compiler
- Linux editor builds: GTK 3 and WebKitGTK 4.1 development packages

Configure and build the plugins:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To run tests:

```sh
ctest --test-dir build --output-on-failure -L '^(core|native)$'
```

Native editor tests require a desktop; Linux CI uses `xvfb-run -a`.
See [test placement and browser setup](tests/README.md) for manual CI and local checks.

Custom builds can change the fixed input, control, output, and parameter-slot counts:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DONDA_PLUGIN_INPUT_CHANNELS=4 \
  -DONDA_PLUGIN_CONTROL_INPUTS=8 \
  -DONDA_PLUGIN_OUTPUT_CHANNELS=8 \
  -DONDA_PLUGIN_PARAMETER_SLOTS=64
```

Main and control inputs together must not exceed 64 channels; outputs support
1 to 64 channels. Set `ONDA_PLUGIN_CONTROL_INPUTS=0` to omit control inputs.
Parameter slots support counts from 1 to 64, matching the automation bank's capacity.

Versions are pinned in [plugin-version](plugin-version),
[onda-version](onda-version), and [juce-version](juce-version).
See [architecture](docs/architecture.md) and [test documentation](tests/README.md)
for implementation and validation details.
