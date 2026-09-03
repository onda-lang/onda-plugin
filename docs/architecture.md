# Architecture and invariants

`Processor` is the JUCE boundary. It owns the permanent parameters, linked
project state with an Onda project image, two one-slot realtime
handoffs, a bounded UI-event queue, and the audio-thread-only active engine
pointer. The VST3 wrapper
adds its format-required `Bypass` parameter outside
the permanent 32-slot Onda set. It also exposes the fixed, non-automatable
controller mappings that VST3 requires to deliver pitch bend, channel pressure,
and CC through JUCE's MIDI buffer.

`Worker` is the sole producer. It serializes native compilation within each
loaded plugin module, which owns one statically linked Onda/LLVM image. Both
source and `.ondaproject` inputs go through `onda_compile_file()`. The worker
hashes Onda's canonical manifest watch projection before and after compilation,
rejects stale generations, retains successful watch coverage across failed
loads, and publishes only fully prepared engines. A successful disk preparation
atomically replaces both the engine and serialized Onda `ProjectImage`. When an
initial disk preparation fails, the worker compiles the complete image in
memory and continues watching the broken disk graph; it never mixes disk and
image contents.

`PreparedEngine` owns, in destruction order, the Onda program, instance,
pointer-stable input/output slabs, decoded interleaved audio buffers, fixed
parameter mappings, and canonical MIDI and host-context event bindings.
Construction validates:

- audio flattens to no more than the compile-time plug-in input and output
  channel counts and every channel is `f32`;
- every buffer has either an Onda-owned immutable project default or an explicit
  `f32` audio-file binding with a compatible channel count;
- canonical MIDI and host-context payload names, order, scalar shapes, and
  types match;
- instruments declare exact `note_on` and `note_off` events.

Audio files are read and decoded entirely on the worker. Their storage remains
stable for the lifetime of the prepared engine and is never touched by the
message thread. Bound paths participate in the same pre/post-build hashing as
source files, so an edit cannot publish a mixed or stale engine. JUCE audio
formats and canonical `.ondabuffer` files feed the same decoded `f32` storage.

After full preparation, Onda captures a source input's successful manifest and
decoded buffers into one immutable, validated project image. For a filesystem
`.ondaproject`, the plugin supplies the exact C-API watch file set to
`onda_project_image_load_files()`; Onda parses the manifest, validates the
workspace, and owns source and asset decoding. Buffer names and absolute
override paths remain in plugin state for live disk loading; the image carries
canonical typed assets for filesystem-free fallback. A disk restore either
prepares every source and buffer or compiles the complete image.

Project export is derived only from that coherent image. Onda owns portable
path mapping, syntax-aware directive rewriting, manifest construction, and
typed asset encoding. The plugin writes the returned materialization plan to a
sibling staging directory, renames it into place, and links the exported
`.ondaproject` manifest. It does not parse or reproduce the manifest's entry and
buffer semantics, and it never partially overwrites a project.

`RunViewHost` owns embedded-resource routing and projection of processor state
into the shared view's host-message schema. `Editor` owns only the native
browser lifetime, periodic publication, and command routing. Processor tests
exercise the editor lifecycle through JUCE's real browser component; resource
bytes and capability state are tested directly through `RunViewHost`.

The view lists user-defined events but omits the canonical `plugin_midi` and
`plugin_host` families, which the host drives. Scalar, fixed-array, and slice
arguments are validated and packed on the message thread into a bounded SPSC
queue. The callback dispatches them at its next boundary without allocation or
locking; a build generation on every command prevents stale event indices from
crossing an engine replacement.

Editor dimensions and the parameter-control layout are processor-owned UI
state. Changes notify the host that non-parameter state is dirty, are serialized
with the project, and are restored before a recreated editor installs its size
constraints. The browser bridge projects the saved layout into the shared view
instead of relying on webview-local storage.

While an editor exists, the callback writes interleaved output into a bounded,
lock-free scope ring owned by `Processor`. The editor snapshots its newest
1,024 frames at 20 Hz and publishes them through the shared view's separate
`scopeData` message. Capturing is disabled when the editor closes; the callback
never allocates, locks, or interacts with the browser.

Host note-on/off state is retained per MIDI channel in atomic bitsets and
published separately as `midiActivity`. The shared keyboard is configured as a
read-only monitor: it exposes no MIDI device or note-input surface, and notes
remain lit while any host channel holds the corresponding key.

The callback captures aliased host input before clearing output. Logical blocks
continue across host callbacks without added latency. JUCE playhead state is
captured once at callback entry. At each new logical block, parameters are
snapshotted first, available host-context fields are dispatched in canonical
order, then same-boundary MIDI is dispatched before audio processing. Missing
JUCE fields produce no context event; `render_mode` is the sole event sent
unconditionally when declared. Prevalidated event-presence metadata gates capture and
dispatch: undeclared MIDI does not split processing, undeclared context fields
do not perform projection or payload work, and an engine without position
events does not query the JUCE playhead.

Host configuration is authoritative. A sample-rate/block-size change makes the
old engine ineligible immediately and requests a new specialization. An
oversized callback never touches undersized slabs; it uses product fallback,
publishes the larger bound atomically, and lets the message-thread timer request
the replacement.

Fallback is silence for the instrument and same-index dry pass-through for the
effect. Double-precision processing is not advertised.

Generated runtime safety checks return a positive status through the Onda C
API. On failure the callback restores fallback audio, requests deactivation,
and publishes only an atomic fault flag. UI status snapshots overlay the
generic failure while that flag is set; no diagnostic formatting or rebuild is
attempted from the audio thread. The faulted engine remains quarantined until a
source change or explicit Reset prepares a replacement.
