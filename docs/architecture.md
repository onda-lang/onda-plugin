# Architecture and invariants

`Processor` is the JUCE boundary. It owns the permanent parameters, linked
project state with an Onda project image, two one-slot realtime
handoffs, a bounded UI-event queue, and the active engine pointer, accessed
only by processing or suspended host preparation. The VST3 wrapper
adds its format-required `Bypass` parameter outside
the fixed `params` set (32 slots by default, configured with
`ONDA_PLUGIN_PARAMETER_SLOTS`) and the control input parameter bank. IDs
and the normalized parameter interface never change.
Bypassed callbacks keep DSP, automation, and MIDI state advancing through the
same processing path. A 5 ms wet/dry crossfade advances once per real sample
after DSP, independently of block and MIDI segmentation. Reversals start at the
current mix; empty callbacks do not advance it. The first rendered callback
settles to its bypass state. Settled bypass returns main dry input for effects
and silence for instruments. The scope captures the audible mixed output.
An immutable presentation snapshot gives mapped slots their compiled Onda
names, units, defaults, discrete step counts, and ABI-backed plain-value text.
Enum descriptors are shared by array elements and snapshots. Host value text and
the shared view use their member labels while DSP and automation retain ordinary
i32 ordinal conversion. Host text parsing prioritizes exact member labels over
identifier aliases and case-insensitive matches. VST3 exposes enums as choice
lists and singleton enums as read-only lists. Member or label changes invalidate
host presentation even when the numeric domain stays unchanged.
The worker schedules a message-thread parameter-info notification only when
that mapping changes; the audio thread never reads presentation metadata.
Bypass queues update the wrapper-owned boolean atomically. Its message-thread
timer delivers coalesced listener notifications with host origin, keeping TLS
initialization and listener locks out of processing and avoiding host edit echoes.
The wrapper also exposes the fixed, non-automatable
controller mappings that VST3 requires to deliver pitch bend, channel pressure,
and CC through JUCE's MIDI buffer.

`ParameterMapping` owns compiled scalar metadata in an independent header. It
describes names, units, ranges, defaults, scale, and discrete steps for both
parameter slots and control inputs. Engine preparation, worker checkpoints,
host parameters, and editor presentation consume the same description.
Unranged numeric parameters use a plugin-owned `[0, 1]` control domain, with a
unit step for integers. This adds no storage range to Onda and preserves exact
authored defaults, including values outside the control interval. Scalar booleans
also map to `[0, 1]`. `ParameterMapping` supplies shared ABI-backed plain/normalized
conversion for DSP and host presentation. The engine writes typed plain values
so both declared and fallback domains use the same update and smoothing paths.
`HostParameter` depends directly on that metadata and `ParameterState`, without
including the DSP engine interface. `ScalarValue` owns numeric-domain and exact
default identity independently of plugin channel configuration.

`Worker` is the sole producer. It serializes native compilation within each
loaded plugin module, which owns one statically linked Onda/LLVM image. Both
source and `.ondaproject` inputs go through `onda_compile_file()`. The worker
hashes Onda's canonical manifest watch projection before and after compilation,
retains the validated disk snapshot through saved-image preparation so edits
during fallback remain visible to polling, rejects stale generations, retains
successful watch coverage across failed loads, and publishes only fully prepared
engines. Native filesystem invalidations trigger content hashing even when size
and modification time are unchanged; fallback polling uses metadata to avoid
unnecessary reads. A successful disk preparation
atomically replaces both the engine and serialized Onda `ProjectImage`. When an
initial disk preparation fails, the worker compiles the complete image in
memory and continues watching the broken disk graph; it never mixes disk and
image contents.

`PreparedEngine` owns, in destruction order, the Onda program, instance,
pointer-stable input/output slabs, decoded interleaved audio buffers, fixed
parameter mappings, and canonical MIDI and host-context event bindings.
Construction validates:

- audio flattens to no more than the combined main/control input capacity and
  output channel count, and every channel is `f32`;
- supplied buffer bindings are Onda-owned immutable project defaults or explicit
  read-only file assets with matching element type and a compatible channel count;
- canonical MIDI and host-context payload names, order, scalar shapes, and
  types match, including rejection of nominal enums in primitive arguments;
- instruments declare exact `note_on` and `note_off` events.

Unbound buffers use Onda's neutral descriptors, matching `onda run`. They do not
block preparation or hide parameters, input controls, events, or MIDI controls;
the program remains active and reads neutral values from those slots.

Audio declarations support individual `f32` scalars, fixed `f32[N]` arrays,
and mixtures of both in `ins` and `outs`. Flattening follows declaration order
and array element index. `N` counts channels at each sample and is fixed at Onda
compilation. Scalar and array ports bind to the same channel-major slabs through
`bindAudio`; arrays require no additional storage, copying, or interleaving.
Port names and grouping do not affect routing. A leading input array covering exactly
the configured main input count occupies those channels; subsequent declarations
map to the control buses.

The main input/output channel counts and optional mono control bus count are
fixed at plugin build time. Control buses default to disabled. The processor
maps JUCE's packed enabled buses to fixed Onda channel positions after the main
inputs; disabled controls have null host pointers. Engine preparation caches
scalar defaults for those positions. Processing copies connected signals and
renders disconnected controls from their stored values or automation into the
existing slabs, with no callback allocations. Input clamping remains Onda's
per-sample responsibility. External signals receive no wrapper smoothing,
normalization, or logarithmic mapping. Only channels declared by the program enter its processing
slab. Dry fallback and bypass use main audio inputs only.
Input routing and pointer shifting also stop at the program's declared input
count; undeclared controls incur no bus lookup, pointer initialization, or
sample copying. A declared control still supplies samples or fills its default,
even if the program's DSP never reads it. Host-side work for an enabled bus is
controlled by the DAW.

The shared view receives a separate host-provided Input Controls collection.
It renders only declared control mappings and marks enabled buses read-only.
The two sections share scalar editors but keep layout, smoothing, and Reset
independent. Manual input edits use `ParameterRamp` to render a plain-unit linear
transition at every sample; automation and external buses bypass this smoothing.
Ramp progression follows real samples across logical blocks and MIDI splits.
Automation leaves the last rendered sample as the next editor ramp's starting
value. External buses invalidate that starting value so disconnecting settles
to the latest stored target. New engines, Reset, and pending seeds also settle
immediately. UI commands, host gestures, and presentation snapshots remain off
the audio thread; state serializes both sections' settings.

Borrowed SDK descriptors expose scalar and array defaults. Preparation decodes
each control element from the same packed default bytes used for parameters;
disconnected channels receive their declared default (zero when absent), clamped
to the declared range.

Audio files are read and decoded entirely on the worker. Their storage remains
stable for the lifetime of the prepared engine and is never touched by the
message thread. Bound paths participate in the same pre/post-build hashing as
source files, so an edit cannot publish a mixed or stale engine. JUCE audio
formats decode to `f32`; canonical `.ondabuffer` files retain their primitive
element type in aligned typed vectors (`bool`, `i32`, `i64`, `f32`, or `f64`).
Parameter arrays flatten in declaration order, with each element taking one
scalar DAW slot. Per-element setters retain sibling and unexposed elements.

Preparation also caches an immutable waveform summary per loaded buffer: up to
128 min/max columns across all channels, ignoring non-finite samples. The run
view receives these summaries through its existing `waveform` field. Project
defaults use the SDK's in-memory materialization plan to obtain canonical typed
assets and their logical bindings; previews never reopen external files or
scan sample storage on the audio or message thread.

After full preparation, Onda captures a source input's successful manifest and
decoded buffers into one immutable, validated project image. For a filesystem
`.ondaproject`, the plugin supplies the exact C-API watch file set to
`onda_project_image_load_files()`; Onda parses the manifest, validates the
workspace, and owns source and asset decoding. The transport workspace uses
the files' common filesystem ancestor so imports outside the manifest's
directory retain their relative paths. Project extension detection matches
Onda's ASCII case-insensitive recognition. Buffer names and absolute
override paths remain in plugin state for live disk loading; the image carries
canonical typed assets for filesystem-free fallback. A disk restore either
prepares every source and buffer or compiles the complete image.
Clearing a buffer records an empty-path binding that explicitly unbinds the slot,
including an immutable project default. The SDK's materialization manifest is
updated and reloaded to remove that binding from the saved/exported image.
Only the root buffer-bindings object is replaced; unrelated manifest bytes stay
opaque, preserving exact compile constants despite JUCE's decimal-parser limits.
Program-owned defaults remain unchanged. Clear intent is saved immediately,
including while a replacement instance is being prepared. Image fallback applies
those explicit unbindings before initialization.

Binding and clearing retain the last valid compiled specialization for the same
source selection, sample rate, block size, and fast-math setting. A replacement
instance shares the immutable program and unchanged read-only file samples.
Each cached file records the content hash of its decoded samples, independently
of watcher snapshots from later attempts. Preparation validates both source and
sample caches before reuse, detecting edits even when native notifications have
not arrived or a previous compilation failed. Failed buffer preparations retain
successfully decoded peers and discard entries whose decoding failed. Bindings
supplied by a saved image are tracked separately, so a failed explicit file
override cannot silently revert to the image default on the next buffer edit.
Saved-image instances retain validated samples when the same linked file becomes
unavailable; explicit clears and path replacements still take effect.
Explicit reloads, source changes, and host specialization changes compile again.
The worker owns the cache and retires instances through the usual handoff, so
shared program and buffer ownership never allocates or destroys on the audio thread.
When a replacement has invalid audio-file bindings, its binding choices are published
separately from the retained engine's audio, parameters, events, and checkpoint.
The editor can bind or clear those pending choices while the previous engine
continues playing. Successful preparation replaces both interfaces together.
Before the first complete checkpoint, source selections and buffer bindings
are published immediately for saving and host dirty notifications. An existing
complete checkpoint takes precedence until its replacement succeeds.

Project export is derived only from that coherent image. Onda owns portable
path mapping, syntax-aware directive rewriting, manifest construction, and
typed asset encoding. The plugin borrows each materialized file for synchronous export and waveform
inspection. Explicit buffer clears retain the plan while replacing only the
manifest bytes. Export writes the returned materialization plan to a
sibling staging directory, renames it into place, and links the exported
`.ondaproject` manifest. It does not parse or reproduce the manifest's entry and
buffer semantics, and it never partially overwrites a project. Every output stream is explicitly
closed and checked before its staging directory can be published.
An export captures its image and request generation before background work
begins. The worker checks both under its mutex before relinking; a newer user
request leaves the exported files intact without changing the current project.

## Native editor

`RunViewHost` projects processor state into the shared
[UI host protocol](https://github.com/onda-lang/onda/blob/main/docs/ui-api.md).
`Editor` routes commands and publications; `GpuView` loads `onda_ui` and owns a
`NativeGpuSurface`. The Rust library owns screen layout, input policy, and drafts;
the plugin owns DAW automation, files, persistence, and DSP.

The surface is a dedicated X11 child, HWND, or NSView inside the JUCE peer.
Only wgpu draws its pixels; input passes through to JUCE for pointer, keyboard,
IME, cursor, and file-drop handling. Peer/movement/visibility changes keep the
child attached. Destroy the renderer before the child or its borrowed display.
The pinned JUCE build notifies child components before desktop peer removal,
matching peer replacement, so an idle view releases its renderer synchronously.
JUCE's native scale notifier schedules presentation for idle host zoom/DPI changes
even when logical bounds stay unchanged; AppKit backing changes also invalidate
the native child directly.
Resize and native exposure schedule a draw through JUCE's event
loop. The child stays hidden until the first frame and hides on rendering errors.
The 60 Hz editor timer polls processor state; visualization has its own 30 Hz
publication budget. Input and state changes request a coalesced redraw through
JUCE's event loop, independently of that polling timer. Sustained input is paced
at a nominal 60 Hz using the host-owned SDK `FramePacer` timer pair. Creation
selects `ONDA_UI_MAILBOX_OR_FIFO` to prefer mailbox presentation with FIFO
fallback; both retain tear-free presentation.
A maximum frame latency hint of two allows CPU/GPU overlap.
The renderer has no software frame limit. Parameter commands still drain
on every event.
A deferred frame retains its deadline across new input, so the latest value
draws without more input and idle views stop waking. The UI advances timed
state without drawing and reports its next wakeup delay. A one-shot view timer
services that deadline, including deferred persistence that needs no frame, and
bounded retries for temporarily unavailable presentation. Hidden views cancel
their wakeups; exposure forces presentation even when UI state is unchanged.

Parameter and input-control values use `parameterValues` after the initial
schema. The shared Rust UI converts normalized values using published domain
metadata; exact scalar values can override them, with `i64` carried as decimal
text. Worker, routing, structural settings, and restored-view changes publish full
state. Smoothing changes acknowledge only the accepted durations and current
error; they do not rebuild or resend interface metadata.
Log history publishes only when its revision changes or a full refresh is needed.

The editor's revision check is lock-free, and the processor timer avoids worker
and preparation mutexes while there is no pending seed, block-size change, or
runtime recovery. Runtime-log draining similarly returns after an atomic
activity check when no output was produced.

Source/project drops retain filesystem paths for imports and live reload. Buffer
picker/drop paths share host-side format, name, and binding validation.

The host owns sample rate. Block preference and fast math can be changed while
loaded or unloaded; changing either setting recompiles a loaded program. The DAW
owns computer MIDI routing, so the editor forwards unused keys and omits
computer-key note input.
JUCE's text interface uses the shared draft/selection/caret geometry. Drain
commands and refresh text state after input and rendering, and before teardown.
This ends gestures and resolves drafts while command handlers still exist.

### Event dispatch and restoration

User events exclude the canonical `plugin_midi` and `plugin_host` families driven
by the host. The message thread validates recursive schemas and packs owned
little-endian payloads into a bounded SPSC queue. The audio callback consumes at
most eight user events per callback, without allocation or locking. Invalid
payloads are non-fatal; handler safety failures quarantine the engine.

Onda's generic `onda_payload_layout` query supplies packed tensor regions and
slice-prefix offsets. Delegate decoding uses the same SDK tensor metadata and
`onda_payload_layout_from_bytes` traversal, with region storage prepared once. The UI encoder writes directly into these regions, while
canonical MIDI/host-context bindings cache their offsets during preparation.
Packed dispatch retains Onda's enum and integer-range normalization. Event
metadata snapshots use an aliasing shared pointer to keep each borrowed plan's
program alive across reloads; queued commands still own their payload bytes.

Event commands carry the rendered engine generation as decimal text. The
processor rejects stale generations before lookup/encoding, and the callback
checks again before dispatch. Filesystem reloads advance this generation too.
New engine generations and explicit argument reset publish `resetEventArguments`;
ordinary updates preserve edits for matching signatures.

Worker schema readiness stays false during waiting, compilation, and retries,
then becomes true on success, terminal failure, or unload. Errors are independent
of status text. A fresh presentation `readyId` restores the same source after
reparenting or host state restoration without replaying ordinary state echoes.
Saved event values require the same source and ordered argument schema/defaults.

Editor size and opaque shared presentation snapshots are processor-owned and
serialized with the project; DSP parameter values remain owned by host automation.
Presentation changes and coherent project/buffer/export/unload changes notify the
host of dirty non-parameter state through the message-thread timer. Identical
recompilation and initial state restoration do not mark the host dirty.

Each published engine owns immutable interface/checkpoint data. The worker
selects the audio thread's active generation when retaining a previous engine
after a failed reload. Replacing an unconsumed queued engine cannot strand a
suspended host; snapshot copying and destruction remain off the audio thread.

## Linked-file watching

The worker uses native filesystem notifications for the exact linked source and
asset graph, filters unrelated sibling activity, and debounces relevant changes
for 200 ms before validating contents. Ancestor subscriptions also detect moves
and deletions, and symlink entries along the resolved path are watched alongside
their target inodes. On macOS, directory notifications compare only child-entry
identities on watched paths; file vnode notifications still invalidate in-place
edits even when their size and modification time are unchanged. When a linked path
disappears, the view reports the load error while the last valid engine and
checkpoint remain in use. Restoring the original path resumes loading.
The worker otherwise blocks on its condition
variable indefinitely. Only paths whose native subscription failed receive a
targeted 1-second disk-validation fallback. Unchanged watch plans retain their healthy native
subscriptions; recovery adds subscriptions only for previously uncovered roots. Directory and
file identities force replacement when a subscribed inode changes.

## Visualization and MIDI ownership

`Processor` owns per-channel peak atomics and the bounded lock-free waveform
ring. `Editor` queries clipped meter and scope visibility independently and
publishes both on one 30 Hz clock. Waveforms use the newest 1,024 frames and
cross the C boundary as floats; the shared UI reduces them before upload.
Meter-only capture skips the ring. Hidden/closed editors disable both captures.
The callback never allocates, locks, or calls UI code for visualization.

Host note state is retained per channel in atomic bitsets and published through
`midiActivity`; a note stays lit while any host channel holds it. OndaSynth's virtual
piano notes use a bounded, generation-scoped queue. Editor commands retain the
generation of the displayed engine; the processor rejects obsolete generations
before queueing and the audio callback checks again before dispatch. Closing the
editor or overflowing the queue releases its holds. Engine adoption clears old
virtual holds, so a late release cannot stop a note in the replacement engine.
MIDI device selection remains host-owned.

## Processing and automation

Onda leaves floating-point mode setup to its host. Each audio callback calls
`onda_configure_current_thread_audio_fp_mode` on its executing thread, including
bypass and fallback. The helper checks the register and enables x86 FTZ/DAZ or
ARM64 FZ only when needed, preserving other control bits. This handles thread
migration and host policy changes without a TLS first-use allocation. The
preparation worker configures its own thread before generated initializers;
`prepareToPlay` leaves the calling host thread's mode unchanged.

The processor keeps host buffer capacity separate from the compiled Onda block
size. An instance preference of zero follows the host; a positive preference is
capped by the host capacity. Settings can change it while loaded or unloaded,
and saved state restores it before specialization. Editor preference changes
retain the working engine until a replacement compiles successfully, including
after a failed build. The run view reports that engine's compiled block size
separately from the requested preference; repairing the source retries the
requested size. Host capacity changes resize scratch storage
without rebuilding an engine whose sample rate and Onda block size still match.
The timer prepares replacement dry-input storage in a one-slot handoff. Only
the audio thread adopts it, at callback entry, and returns the old storage for
message-thread destruction. Smaller callbacks can keep rendering during
preparation; a larger callback uses fallback until adequate storage is adopted.
Suspended host preparation can replace and reclaim scratch storage directly.

Fast Math is an instance compilation preference, off in a new instance and
editable in Settings. Saved state restores it before specialization. Every
worker request captures the option for both disk and saved-image compilation,
including automatic reloads and host specialization changes.

The callback captures aliased host input before clearing output. Each callback
is divided into logical blocks of at most the compiled Onda block size. Its
final block ends on the last real segment, even when shorter than that maximum;
the next callback starts a fresh block without padding, an extra native call,
or resetting DSP state. MIDI splits leave the current block open. JUCE playhead
state is captured once at callback entry. At each new
logical block, parameters are snapshotted first, available host-context fields
are dispatched in canonical order, then same-boundary MIDI is dispatched before
audio processing. Missing JUCE fields produce no context event; `render_mode`
is the sole event sent unconditionally when declared. Prevalidated event-presence
metadata gates capture and
dispatch: undeclared MIDI does not split processing, undeclared context fields
do not perform projection or payload work, and an engine without position
events does not query the JUCE playhead.

**SDK limitation: `BLOCK_SIZE` and short logical blocks.**
Onda's `BLOCK_SIZE` (`BS`) remains the compile-time block capacity, even when a
callback's final chunk is shorter. With capacity 64, a 190-sample callback runs
chunks of 64, 64, and 62 samples, but `BLOCK_SIZE` is 64 in all three. The UI
offers Host and rounded-down half, quarter, and eighth sizes. With usual
power-of-two host capacities and callbacks matching that capacity, these
choices produce full chunks during steady playback.
Array sizing and sample-by-sample DSP remain valid. Authored block-rate clocks,
smoothing, and oscillators that advance by `BS` can overcount elapsed samples;
the pinned SDK's `std::osc::KSine` uses `freq * BS / SR`. Algorithms requiring
complete fixed-size windows also need to account for partial chunks.
In the 190-sample example, a clock advancing by `BS` counts 192 samples. A
constant 32-sample callback with capacity 64 makes that clock run twice as fast.
Occasional loop-boundary tails usually cause small, accumulating timing errors;
frequent short callbacks can cause appreciable drift. The plugin's editor
smoothing already advances by actual chunk length.

Until the SDK exposes the actual chunk length, authored timing can advance in
`sample` using `SAMPLE_RATE`; a `block` hook cannot derive elapsed samples from
`BLOCK_SIZE`. Host callback lengths and MIDI segment lengths are distinct: the
elapsed count needed by a block hook must describe the complete logical chunk.

Follow-up work:

- [ ] Expose the actual logical-block sample count through the Onda SDK/compiler,
  retaining `BLOCK_SIZE` for compile-time capacity and array dimensions. The
  count must describe the whole chunk and stay unchanged across MIDI segments.
- [ ] Update `KSine` and other block-rate timing helpers to use actual elapsed
  samples.
- [ ] Add authored block-rate timing regressions for shortened callbacks, host
  sizes not divisible by Onda size, MIDI splits, and zero-sample event callbacks.
  Existing callback and smoothing tests already cover the wrapper's scheduling
  and actual-sample ramp progression.

`ParameterRamp` owns the plain-unit linear transition for each continuous float
slot. `PreparedEngine` converts normalized editor targets through Onda's ABI and
publishes one value before block events and audio, advancing by that logical
block's actual length, including short callback tails. MIDI segmentation does
not advance ramps again. Event-only
callbacks do not advance ramps. Retargeting starts from the
last value written to DSP, including f32 rounding. Discrete controls bypass the
ramps. `Processor` retains the smoothing duration atomically, defaults to 30 ms,
and serializes it in plugin state; the shared view edits it without recompiling
or resetting DSP. Zero or a duration at most one configured block settles
targets directly. Initialization, replacement, and reset also settle immediately.

`ParameterState` keeps each permanent slot's normalized JUCE float, editor/host
origin, exact-default identity, and 32-bit write sequence in one lock-free atomic
word. Normalized floats leave their sign bit and one exponent bit available for
the origin and default tags, including canonicalized positive zero.
Exact host endpoints live in a double cache keyed by the complete
atomic word; any newer float write invalidates that cache, including an identical
value. Only audio writes the cache. Its payload and stamp are lock-free atomics,
and a publication marker covers the interval between the conditional value
commit and the completed endpoint. Worker and saved-state snapshots retry
overlapping publications or float writes to retain the exact value with its
stamp. Audio reads the cache once without retrying. Failed conditional commits
restore the previous cache stamp without changing its payload. Queue publication
and seeding each attempt one compare-exchange and never overwrite a newer write.
The sequence must not wrap within a pending handoff (over four billion writes to
the same parameter).
Parameters are registered directly with JUCE; there is no mirrored APVTS state.
Editor origin is recorded before notifying the host. Only editor targets use
`ParameterRamp`; delivered host changes settle directly and cancel active ramps.
Settled values skip redundant DSP writes. Continuous targets are converted to
plain units once and written through the typed setter; ramp cancellation and
disabling smoothing still publish the target immediately at the next boundary.
Prepared engines cache each parameter's SDK conversion domain, so automation
and smoothing retargets do not rebuild type/scale metadata on the audio thread.

The build generates an adapted copy of the pinned JUCE VST3 wrapper; dependency
checkouts are unchanged and configuration requires exactly one match per hook.
`ScopedVst3Automation` borrows full host queues, publishes managed endpoints
atomically, and clears borrowed data on every return. The adapted wrapper skips
JUCE endpoint listener calls for Onda parameters, while preserving its bypass
and MIDI handling. Coalesced parameter notifications run on the message thread.
Controller plain/normalized conversions use the same Onda mappings as DSP,
including discrete parameters and constant-range input controls; unmapped slots
and bypass retain their normalized domains. Discrete VST3 step counts come
directly from compiled metadata, capped at the format's signed 32-bit maximum;
they do not pass through JUCE's `INT_MAX` continuous sentinel. Successful
selection seeds and new control defaults also request one coalesced
`kParamValuesChanged` refresh after
controller delivery. That refresh runs only from the message-thread timer,
outside host preparation, using JUCE's program-change notification. Incoming
automation updates controller presentation without requesting a value refresh
or reporting an edit to the host.
Offline synchronization and host preparation leave those notifications queued
until message-thread delivery; their DSP endpoint commits remain synchronous.
After deactivation is requested or an engine is quarantined, queue capture and
publication use host values without the outgoing engine's seed barrier or editor
ramp context, including parameter-only flushes before engine retirement.
Their host origin is scoped to the notified parameter and retained through
controller synchronization, which neither writes rounded values back to DSP
nor reports plugin edits. Nested editor notifications establish their own origin.
The exported JUCE wrapper retains its ordinary callback mutex; managed automation
adds no mutex acquisitions or allocations. `ParameterAutomation` reconstructs
normalized linear sections, including the implicit previous-callback endpoint,
at sample offset -1, or holds discrete points. Control curves convert those
normalized samples through their linear input range. Ordinary continuous
parameters apply their declared linear/logarithmic/curve mapping after sampling
the normalized curve at a logical block start.
Each automation bank tracks its borrowed queues and deferred endpoints in
64-bit masks. Clearing, publication, and message-thread notification visit only
set bits; empty banks perform no work proportional to their capacity. Undeclared
lanes retain their last valid endpoint without interpolating or rendering the curve.
Within a callback, unchanged seed revision, default selection, and acknowledgment
reuse the previous publication. New queues or a changed seed context reconcile
again, retaining adoption and concurrent-write protections.
Only logical block starts evaluate parameter curves, including callback entry.
Automation points and MIDI do not introduce extra block boundaries; control
curves and routed buses retain their per-sample values across every chunk and
MIDI split. MIDI still dispatches at its sample offsets. Queue
endpoints retain double precision for DSP and subsequent curve reconstruction;
the JUCE parameter interface remains float. State format version 12 stores
normalized slot and control values in double precision and preserves exact
scalar defaults as tagged floating or integer values with their original numeric
domains, alongside the block-size preference. Unranged `i64` parameters retain
their full-width defaults through Onda's raw default API, including Reset,
initialization, reload, and state restoration; normalized writes use the
plugin's 0–1 control domain.
Unloaded state restoration retains the same stamped scalar snapshot. Project-only
updates, including another unload or an incomplete selection, preserve it until
a newer host/editor write invalidates its stamp. Loading while preserving host
values therefore supplies the exact restored values to initialization and DSP.

Position projection also captures whether the timeline is playing, independently
of whether a transport event is declared. Stopped sample, time, and musical
positions remain unchanged at interior logical-block boundaries.

Each worker build attempt snapshots the host slots once in double precision and
applies the mapped values before full initialization, including pinned
initializers. Disk builds and saved-image fallback use the same snapshot. New
patch selections instead use declared defaults. A seed belongs to its engine and
checkpoint; publishing
it does not deactivate the playing engine or change host slots. Adoption commits
the seed. Until the processor finishes conditionally writing every host slot
and acknowledges it with an atomic release, the adopted engine reads its
prepared defaults at logical block boundaries. Each callback retains its
initial seeding decision,
so acknowledgment cannot enable an old borrowed curve halfway through it.
Queue endpoints received during this interval are retained in double precision
and published at the first callback after acknowledgment; newer endpoints and
editor targets take precedence. They supply the next curve's implicit starting
value and follow inherited seeds across reloads, but never cross a new patch
selection.
Adoption records each slot's write stamp. Acknowledgment records successful seed
commits, allowing retained endpoints to follow those writes while newer direct
host writes and editor edits remain authoritative. Borrowed curves also carry
their publication stamp, so a later edit supersedes them at subsequent processing
boundaries. Parameter notifications run on the message thread after DSP commits
them, so host/controller state catches up without audio-thread listener work.
The message-thread timer seeds only an adopted engine,
even if a newer request is compiling. Superseding an unadopted replacement leaves
the playing engine and its parameters intact. A replacement preserving host
values inherits an unfinished seed, so initialization and processing cannot
observe partially written defaults.
Each seed commit publishes its proposed provenance stamp before the conditional
value write, and clears it on failure. Worker snapshots therefore recognize
the exact scalar on either side of a successful write, before acknowledgment.
The seed's revision identifies its patch selection across reloads and host
re-specialization; acknowledgment completes that selection even when its engine
generation has advanced.
Saved state snapshots capture the checkpoint and current host slots together,
substituting defaults still pending for that checkpoint unless a newer direct
write already supersedes them. These defaults survive request supersession.
After acknowledgment, control defaults retain their exact scalar value while
the successful seed's write stamp still matches. Ordinary slots retain the same
exact-default identity until a newer host or editor write supersedes it.
Snapshot capture is serialized with seeding and host state
restoration. Initializers remain on the worker, while subsequent parameter
automation takes effect at the next existing logical block boundary.

Host preparation waits on the worker's completion condition before returning
and adopts the specialization without needing a GUI timer tick. An unchanged
configuration retains its eligible engine. Offline callbacks also synchronize
when state is restored after preparation or the host changes block bounds.
The preparation mutex serializes these non-realtime paths with timer-driven
parameter seeding, scratch publication, and scratch reclamation; realtime
callbacks never acquire it.
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
The exported wrapper filters SysEx before JUCE's VST3 conversion as well.
One callback accepts at most 4,096 MIDI event-list entries and MIDI-controller
queue points combined, using storage reserved during preparation. JUCE retains
conversion and stable event ordering. On overflow, the entire MIDI batch is
discarded and the processor releases its held host notes before rendering,
without resetting DSP or changing ordinary parameter/control automation. A
parameter-only overflow defers those releases until the next audio callback.

Generated runtime safety checks return a positive status through the Onda C
API. Prepared-instance resets use `onda_init_checked`, whose success and failure
paths return numeric status without allocating diagnostics, then refresh process
readiness with `onda_prepare_process`. On failure the callback
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

Control input automation uses the same borrowed queue cursor, normalized value
storage, and seed barrier as ordinary parameters. Metadata preparation caches
input names and linear ranges; it publishes host parameter and bus titles on
the message thread. The audio callback renders disconnected controls directly
into the existing Onda input slabs. Each linear span caches its reciprocal
duration and interpolates normalized values before range conversion, preserving
zero crossings in wide bipolar ranges. Constant spans use a bulk fill. This
adds no scratch buffers, allocations, locks, or logical block boundaries.
Both editor ramps and control curves select their endpoint-safe interpolation
arithmetic outside the sample loop. Editor ramps batch their elapsed-state
updates and retain division for their absolute sample weights, preserving the
rounding of scalar advancement. Control curves select range conversion once per
render call and normalized interpolation once per host-point span. Neither path
accumulates sample increments or combines normalized interpolation with scaling.
Enabled control buses supply routed samples
instead and skip cursor preparation and target conversion; their host parameter
endpoints still advance.
Disconnected inputs prepare their cursor or constant target once, when the
callback first renders samples. Input automation bypasses
ordinary editor parameter smoothing and persists with the project checkpoint.
When a reload first declares an untouched control, its exact input default is
included in the immutable checkpoint. Adoption conditionally initializes its
host value, without extending the selection's seed barrier. Existing writes and
retained automation endpoints take precedence, including a first queue that
repeats the bank's initial `0.5` value. The committed stamp is published before
the value, so DSP and saved-state readers retain the matching exact scalar.
Controller notifications remain on the message thread.
A newer target that cancels a borrowed curve uses the same control-value
conversion as a constant input, including exact scalar defaults after Reset.
While seeding is pending, disconnected controls convert the inherited seed's
normalized values through their current ranges. Reloaded declarations cannot
replace those values before acknowledgment; newly declared controls outside
the inherited seed use their prepared defaults.

`ScalarValue`, shared by ordinary slots and control inputs, keeps a scalar
default's exact plain value and original numeric domain in immutable
seed/checkpoint data. One private engine resolver applies the same Reset,
acknowledged-seed, checkpoint, and normalized-target precedence to both banks.
Normalization alone cannot preserve arbitrary scalar defaults, even with double
precision. Engine adoption borrows the checkpoint's
stamped values; unchanged domains reuse the exact scalar, while changed domains
map its normalized value through the new declaration. A slot domain includes
its type, range, scale, curve, and step metadata, and whether the interval is
an implicit UI range that permits an unbounded authored default. This keeps
fallback domains distinct from an explicit Onda range with the same bounds. Reset
records the defaults it actually wrote, including resets during compilation.
`ParameterBank` publishes Reset's exact scalar, original domain, and write stamp
through one shared lock-free cache implementation, independently of engine
adoption. Audio reads that
cache once; a concurrent publication retains the engine's previous complete
control value. The engine caches each complete Reset snapshot by its write
stamp, so unchanged defaults reuse their scalar and domain without rereading
the atomic cache at subsequent block boundaries. Incomplete snapshots remain
eligible for another read. This cache is separate from the mutable fallback
used by seeding and automation. The engine never infers Reset's scalar from a
normalized float or the active declaration.
Direct host writes and incoming queue endpoints invalidate that identity even
when their normalized float is identical. State restoration retains this
metadata without introducing a new seed barrier.

Editor echo filtering applies to continuous parameter slots and input controls
with a pending editor ramp. It scans queue points until a value differs from
the editor target; an all-matching queue preserves the ramp. Unmapped lanes and
settled targets inspect only queue endpoints until a curve is rendered. Seed
notifications for both banks are coalesced and delivered on the message thread,
including offline processing and host
preparation.

## Deferred input-control metadata

Support for `scale` (`linear` or `log`), `curve`, `step`, and `unit` on Onda
`ins` is an open design question tracked in the
[Onda language TODO](https://github.com/onda-lang/onda/blob/main/docs/todo/language.md#language-follow-ups).
For now, input controls use declared defaults and min/max ranges with linear
automation mapping. Authors can implement other scaling or quantization
explicitly in the `sample` block; connected signals retain the existing Onda
range-clamping behavior.

Revisit plugin support after Onda settles the relationship between control
metadata and sample-rate inputs. If adopted, consume shared domain metadata
and conversion helpers, and measure sample-accurate nonlinear automation with
flat and changing spans, sparse and dense queues, and processing-segment
splits. Any host-side control mapping must leave externally supplied signal
samples untransformed.
