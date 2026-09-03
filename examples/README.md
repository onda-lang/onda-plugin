# Plugin examples

These patches are designed for the VST3 host integration rather than Onda's
standalone example runner:

- `instruments/poly_saw.onda` is an eight-voice saw, filter, and saturation
  instrument. Load it in `OndaSynth`; MIDI note on/off and per-channel pitch
  bend are delivered through the plug-in's canonical events.
- `instruments/fm_bells.onda` is an eight-voice velocity-sensitive FM bell bank
  with ADSR articulation, an inharmonic partial, a filtered strike transient,
  and cross-fed stereo ambience.
- `effects/tempo_ping_pong.onda` is a filtered cross-feedback delay. Load it in
  `OndaFX`; the optional `tempo(bpm: f64)` event follows the DAW tempo and
  retains 120 BPM until the host supplies one.
- `effects/reactive_wavefolder.onda` is an oversampled stereo wavefolder whose
  fold depth and brightness follow the input envelope. Load it in `OndaFX`.
- `effects/transient_sculptor.onda` separates fast attacks from the slower body
  of a sound and provides independently bipolar attack and sustain controls.
- `effects/orbit_flanger.onda` uses quadrature delay modulation and filtered
  stereo cross-feedback for a wide, animated flanging field.

Open a patch from the corresponding plug-in's embedded run view. Its top-level
parameters are mapped to the host's permanent automation slots when the patch
becomes active.
