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

The plugin provides 32 DAW automation slots for `params`. Mapped slots use the
loaded program's parameter names and units. Slots remain stable when the
program is recompiled.

Scalar numeric parameters without a declared range appear in **Params** and map to
DAW slots using a plugin-owned 0–1 control range. This adds no range to the Onda
declaration and preserves authored defaults. Declare an explicit range when
controls should cover other values, for example `frequency = 440.0 {20.0, 20000.0}`.
Integer controls use whole-number steps; boolean controls use toggles.

Continuous `params` automation follows the DAW's normalized linear curve,
sampled at the start of each Onda block. Discrete parameters hold the latest
point at that boundary. Automation points and MIDI events do not create extra
Onda blocks.

**Smooth**, in the Params header, smooths continuous float parameter changes
made in the editor. It defaults to 30 ms; set it to 0 to disable it. Integer,
boolean, and stepped parameters are not smoothed. Transitions are linear in
plain units and update once per Onda block. A duration of one configured block
or less applies immediately.

DAW automation normally bypasses smoothing. While an editor transition is
pending, a DAW automation queue whose points all match the editor target is
treated as feedback, preserving the transition. Genuine automation with the
same values is indistinguishable from that feedback and also preserves the
transition.

Bypass transitions use a 5 ms crossfade to the main audio input in OndaFX and
silence in OndaSynth. DSP, MIDI, and automation keep advancing during bypass.

Choose an **Onda Block Size** before loading a program. **Host** follows the
DAW's configured buffer size; shorter DAW buffers are always respected.
Smaller blocks update `params` automation and editor smoothing more frequently
and use more CPU. Control input automation remains sample-accurate
at every block size. Smoothing and block-size settings are saved with your DAW
project.

The final Onda block in a DAW callback can be shorter than the configured size.
Onda's `BLOCK_SIZE` (`BS`) still reports the configured size, so authored clocks,
smoothers, and block-rate oscillators such as `std::osc::KSine` can run ahead if
they advance time by `BS`. Use per-sample timing for those algorithms when short
callbacks are possible. The plugin's editor smoothing uses the actual sample
count. See [the architecture notes](docs/architecture.md) for the SDK follow-up.

## Audio inputs and outputs

The default layout has two main audio inputs and two outputs. Declare channels
as individual `f32` ports or `f32[N]` arrays, where `N` is the channel count.

## Control inputs

The default build provides 16 optional mono control inputs. In the stereo
layout, declare the two main input channels first; subsequent inputs map to
`Control 1`, `Control 2`, and so on. See
[external_control.onda](examples/effects/external_control.onda) for an example.

Each control appears as a named, automatable DAW parameter. Its bus starts
disabled, and parameter automation supplies its value at every sample. To use
an external signal, enable and route the corresponding bus through your DAW's
sidechain or auxiliary inputs. The routed signal takes precedence; disabling
the bus returns to the latest parameter value, including automation received
while the bus was enabled. Control input automation and routed signals bypass
the **Smooth** setting.

The editor's **Input Controls** section shows the controls declared by the
loaded program. It has independent **Sliders/Knobs**, **Smooth**, and **Reset**
controls. Its Smooth setting defaults to 30 ms and smooths manual edits in plain
units at every sample; set it to 0 to disable it. Reset restores input defaults
without resetting Params. With an external bus enabled, its editors are disabled
and show **External input**. Layout and smoothing settings are saved in the DAW
project.

Controls start at their declared input default, or zero. Declare an input range
to set the values covered by automation, which maps linearly to that range.
Routed signals are passed directly to Onda. Use scalar control inputs when you
need defaults; control arrays must omit defaults.

For different scaling or quantization, apply the conversion explicitly in the
`sample` block. Additional input-control metadata is
[deferred](docs/architecture.md#deferred-input-control-metadata).

## Audio buffers

Bind audio files to named buffers in the editor. Supported formats include WAV,
AIFF, FLAC, Ogg Vorbis, and `.ondabuffer`. Audio-file bindings require read-only
`f32` buffers. An `.ondaproject` can also supply buffer defaults; clearing a
binding restores its project default when available.

Linked source and audio files reload automatically after edits. Failed reloads
keep the previous working program playing; missing required buffers must be
bound before a new program can run.

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
ctest --test-dir build --output-on-failure
```

Custom builds can change the fixed input, control, and output channel counts:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DONDA_PLUGIN_INPUT_CHANNELS=4 \
  -DONDA_PLUGIN_CONTROL_INPUTS=8 \
  -DONDA_PLUGIN_OUTPUT_CHANNELS=8
```

Main and control inputs together must not exceed 64 channels; outputs support
1 to 64 channels. Set `ONDA_PLUGIN_CONTROL_INPUTS=0` to omit control inputs.

Versions are pinned in [plugin-version](plugin-version),
[onda-version](onda-version), and [juce-version](juce-version).
See [architecture](docs/architecture.md) and [test documentation](tests/README.md)
for implementation and validation details.
