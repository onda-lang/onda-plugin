# Architecture and invariants

`Processor` is the JUCE boundary. It owns the permanent parameters, linked
project state with an Onda project image, two one-slot realtime
handoffs, a bounded UI-event queue, and the active engine pointer, accessed
only by processing or suspended host preparation. The VST3 wrapper
adds its format-required `Bypass` parameter outside
the permanent 32-slot Onda set. Slot IDs and normalized storage never change.
An immutable presentation snapshot gives mapped slots their compiled Onda
names, units, defaults, discrete step counts, and ABI-backed plain-value text.
The worker schedules a message-thread parameter-info notification only when
that mapping changes; the audio thread never reads presentation metadata. The
wrapper also exposes the fixed, non-automatable
controller mappings that VST3 requires to deliver pitch bend, channel pressure,
and CC through JUCE's MIDI buffer.

`Worker` is the sole producer. It serializes native compilation within each
loaded plugin module, which owns one statically linked Onda/LLVM image. Both
source and `.ondaproject` inputs go through `onda_compile_file()`. The worker
hashes Onda's canonical manifest watch projection before and after compilation,
retains the validated disk snapshot through saved-image preparation so edits
during fallback remain visible to polling, rejects stale generations, retains
successful watch coverage across failed loads, and publishes only fully prepared
engines. A successful disk preparation
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
  read-only `f32` audio-file binding with a compatible channel count;
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
Clearing a required binding invalidates the complete image but retains the
source link and remaining binding paths in saved state, even while preparation
is incomplete.
When a replacement is missing buffers, its binding choices are published
separately from the retained engine's audio, parameters, events, and checkpoint.
The editor can bind or clear those pending choices while the previous engine
continues playing. Successful preparation replaces both interfaces together.
Before the first complete checkpoint, source selections and buffer bindings
are published immediately for saving and host dirty notifications. An existing
complete checkpoint takes precedence until its replacement succeeds.

Project export is derived only from that coherent image. Onda owns portable
path mapping, syntax-aware directive rewriting, manifest construction, and
typed asset encoding. The plugin writes the returned materialization plan to a
sibling staging directory, renames it into place, and links the exported
`.ondaproject` manifest. It does not parse or reproduce the manifest's entry and
buffer semantics, and it never partially overwrites a project. Every output stream is explicitly
closed and checked before its staging directory can be published.
An export captures its image and request generation before background work
begins. The worker checks both under its mutex before relinking; a newer user
request leaves the exported files intact without changing the current project.

`RunViewHost` owns embedded-resource routing and projection of processor state
into the shared view's host-message schema. `Editor` owns the native browser
lifetime, periodic publication, and command routing. Processor tests
exercise the editor lifecycle through JUCE's real browser component; resource
bytes and capability state are tested directly through `RunViewHost`.
Windows explicitly uses WebView2 with a writable per-user data folder, and its
editor lifecycle test requires the native browser's ready handshake.
The editor publishes its initial state on `webviewReady` and keeps the loading
overlay until the shared view responds with `runViewReady`, after rendering the
host state and restoring saved controls and the document scroll position.
Each restore carries a `readyId` so an acknowledgment from an older render
cannot uncover a newer one.

The view lists user-defined events but omits the canonical `plugin_midi` and
`plugin_host` families, which the host drives. The host consumes Onda's
recursive event schemas, including nested structs, tuples, fixed arrays, and
struct slices. Arguments are validated and packed into Onda's little-endian
structure-of-arrays wire format on the message thread before entering a bounded
SPSC queue. Onda's flat event-tensor metadata is checked against the recursive
schema plan when an engine is prepared. The native borrowed-view entry point is
not used because values cross both UI/audio threads and the queue; the packed
entry keeps ownership explicit and dispatch allocation-free. The callback
dispatches events at its next boundary without allocation or locking; payload
rejection returns an empty execution-output batch and remains non-fatal, while
handler safety failures quarantine the engine. Structured delegate records are
decoded from the same schema into the runtime log. A build generation on every
command prevents stale event indices from crossing an engine replacement.
Automatic filesystem reloads advance this generation as well as explicit
requests. The worker atomically replaces and destroys an unconsumed queued
engine, so a suspended host cannot cause repeated compilation or prevent
preparation from completing.
Each published engine owns an immutable snapshot of its interface and project
checkpoint. The worker keeps weak references to these snapshots and selects
the audio thread's active generation when retaining a previous engine after a
failed reload. This preserves the matching checkpoint even if adoption overlaps
another build. Snapshot copying and destruction stay off the audio thread.

Ordinary view updates send event defaults as metadata without overwriting the
arguments edited in the browser. A new engine generation initializes the
arguments; the explicit argument-reset command restores their defaults. Both
send the shared view's `resetEventArguments` state flag, so even an incomplete
draft is discarded when the canonical value has not changed. Unchanged event
controls retain their DOM nodes, focus, and caret across ordinary updates.
Focused numeric parameter fields retain their drafts until commit or blur;
leaving a field without committing reconciles the latest host value.
Log history is included only when its revision changes or a full view refresh
is requested, so parameter automation does not repeatedly serialize old logs.

Editor dimensions and the parameter-control layout are processor-owned UI
state. Changes notify the host that non-parameter state is dirty, are serialized
with the project, and are restored before a recreated editor installs its size
constraints. The browser bridge projects the saved layout into the shared view
instead of relying on webview-local storage. Coherent project-image publication,
buffer changes, export relinking, and unload also notify the host that saved
state changed. Identical recompilation and initial host-state restoration do not
mark the host dirty. Project-state notifications are delivered by the
message-thread timer.

The shared view reports its page and log scroll positions, keyboard octave and
velocity, section and argument folds, and event argument drafts as one bounded
snapshot.
The processor retains that snapshot per plugin instance, serializes it with the
project, and includes it with the first state sent to each recreated browser or
after a host state restore in an open editor.
Event values are matched to their source path and argument schema when restored;
audio parameters remain owned by the processor and host automation.

The editor's revision check is lock-free, and the processor timer avoids worker
and preparation mutexes while there is no pending seed, block-size change, or
runtime recovery. Runtime-log draining similarly returns after an atomic
activity check when no output was produced.
The worker uses native filesystem notifications for the exact linked source and
asset graph, filters unrelated sibling activity, and debounces relevant changes
for 200 ms before validating contents. It otherwise blocks on its condition
variable indefinitely. Only paths whose native subscription failed receive a
targeted 500 ms disk-validation fallback.

While an editor is showing, the callback writes interleaved output into a bounded,
lock-free scope ring owned by `Processor`. Reset generations avoid a
read-modify-write in steady-state processing, and the editor only snapshots
and publishes the scope after its frame revision changes. The snapshot uses its
newest 1,024 frames at 20 Hz and publishes them through the shared view's
separate `scopeData` message. Capturing is disabled when the editor closes or
JUCE reports it hidden. The callback never allocates, locks, or interacts with
the browser.

Host note-on/off state is retained per MIDI channel in atomic bitsets and
published separately as `midiActivity`. Notes remain lit while any host channel holds the corresponding key.
OndaSynth also accepts the shared keyboard's virtual note commands through a
bounded SPSC queue, dispatched on MIDI channel 1 at the next callback. Commands
are scoped to the compiled engine generation. Closing the editor or overflowing
the queue releases its held notes. MIDI device selection remains host-owned.

The callback captures aliased host input before clearing output. Logical blocks
continue across host callbacks without added latency unless host automation
changes; then the unfinished block ends before the next callback's events and
audio. JUCE playhead state is captured once at callback entry. At each new
logical block, parameters are snapshotted first, available host-context fields
are dispatched in canonical order, then same-boundary MIDI is dispatched before
audio processing. Missing JUCE fields produce no context event; `render_mode`
is the sole event sent unconditionally when declared. Prevalidated event-presence
metadata gates capture and
dispatch: undeclared MIDI does not split processing, undeclared context fields
do not perform projection or payload work, and an engine without position
events does not query the JUCE playhead.
Position projection also captures whether the timeline is playing, independently
of whether a transport event is declared. Stopped sample, time, and musical
positions remain unchanged at interior logical-block boundaries.

Each worker build attempt snapshots the host slots once and applies the mapped
values before full initialization, including pinned initializers. Disk builds
and saved-image fallback use the same snapshot. New patch selections instead
use declared defaults. A seed belongs to its engine and checkpoint; publishing
it does not deactivate the playing engine or change host slots. Adoption commits
the seed. Until the processor finishes writing every host slot and acknowledges
it with an atomic release, the adopted engine reads its prepared defaults at
logical block boundaries. The message-thread timer seeds only an adopted engine,
even if a newer request is compiling. Superseding an unadopted replacement leaves
the playing engine and its parameters intact. A replacement preserving host
values inherits an unfinished seed, so initialization and processing cannot
observe partially written defaults.
Saved state snapshots capture the checkpoint and current host slots together,
substituting defaults still pending for that checkpoint. These defaults survive
request supersession and stop overriding saved values once seeding is
acknowledged. Snapshot capture is serialized with seeding and host state
restoration. Initializers remain on the worker, while subsequent parameter
automation starts a new logical block at the next host callback when needed.

Host preparation waits on the worker's completion condition before returning
and adopts the specialization without needing a GUI timer tick. An unchanged
configuration retains its eligible engine. Offline callbacks also synchronize
when state is restored after preparation or the host changes block bounds.
The preparation mutex serializes these non-realtime paths with timer-driven
parameter seeding and scratch resizing; realtime callbacks never acquire it.
Offline processing also reconciles block bounds raised by a previous oversized
realtime callback before waiting for the specialization.
Synchronous preparation may reclaim a queued retirement directly; ordinary
realtime retirement remains the worker's responsibility.

Host configuration is authoritative. A sample-rate/block-size change makes the
old engine ineligible immediately and requests a new specialization. An
oversized realtime callback never touches undersized slabs; it uses product fallback,
publishes the larger bound atomically, and lets the message-thread timer request
the replacement.

Fallback is silence for the instrument and same-index dry pass-through for the
effect. Double-precision processing is not advertised. A loaded program advertises
an infinite VST3 tail because arbitrary DSP may sustain indefinitely; an unloaded
plugin reports no tail. MIDI filtering rejects unsupported system/SysEx and
malformed channel packets before constructing an owning JUCE message.

Generated runtime safety checks return a positive status through the Onda C
API. Prepared-instance resets use `onda_init_unchecked` so initializer failures
also return a status without allocating diagnostics. On failure the callback
restores fallback audio, quarantines the active instance, clears its published
active generation, and publishes an atomic fault flag. Quarantine belongs to
the audio-owned engine and does not change the worker's lifecycle deactivation
flag: a late failure cannot strand an already prepared replacement. The next
callback retires the faulted engine through the ordinary off-thread handoff.
UI status snapshots overlay the
generic failure while that flag is set; no diagnostic formatting or rebuild is
attempted from the audio thread. The faulted engine remains quarantined until a
source change or explicit Reset prepares a replacement.
Reset applies current host parameter values before rerunning initializers,
including the defaults written by the editor's Reset command.
