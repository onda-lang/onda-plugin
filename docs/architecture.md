# Architecture and invariants

`Processor` is the JUCE boundary. It owns the permanent parameters, linked
project state with an Onda project image, two one-slot realtime
handoffs, a bounded UI-event queue, and the active engine pointer, accessed
only by processing or suspended host preparation. The VST3 wrapper
adds its format-required `Bypass` parameter outside
the permanent 32-slot `params` set and the control input parameter bank. IDs
and the normalized parameter interface never change.
Bypassed callbacks keep DSP, automation, and MIDI state advancing through the
same processing path. A 5 ms wet/dry crossfade advances once per real sample
after DSP, independently of block and MIDI segmentation. Reversals start at the
current mix; empty callbacks do not advance it. The first rendered callback
settles to its bypass state. Settled bypass returns main dry input for effects
and silence for instruments. The scope captures the audible mixed output.
An immutable presentation snapshot gives mapped slots their compiled Onda
names, units, defaults, discrete step counts, and ABI-backed plain-value text.
The worker schedules a message-thread parameter-info notification only when
that mapping changes; the audio thread never reads presentation metadata. The
wrapper also exposes the fixed, non-automatable
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
- every buffer has either an Onda-owned immutable project default or an explicit
  read-only `f32` audio-file binding with a compatible channel count;
- canonical MIDI and host-context payload names, order, scalar shapes, and
  types match;
- instruments declare exact `note_on` and `note_off` events.

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

The pinned SDK exposes scalar input defaults but cannot read array defaults.
An array default overlapping control channels is rejected during preparation;
arrays without defaults remain valid and disconnected channels receive zero.

Audio files are read and decoded entirely on the worker. Their storage remains
stable for the lifetime of the prepared engine and is never touched by the
message thread. Bound paths participate in the same pre/post-build hashing as
source files, so an edit cannot publish a mixed or stale engine. JUCE audio
formats and canonical `.ondabuffer` files feed the same decoded `f32` storage.

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
Clearing a required binding invalidates the complete image but retains the
source link and remaining binding paths in saved state, even while preparation
is incomplete.
Clearing an override with a project default retains the engine, interface, and
complete checkpoint until the default-backed replacement succeeds. Canonical
assets captured from a linked source preserve its file bindings during restore;
they do not turn required source bindings into project defaults.
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
The pinned run view is copied and patched during configuration; a mismatched
patch fails the build. Host publications and unedited blur retain plain values
without applying the editor's range or step grid. Exact `i64` defaults cross
the bridge as decimal text, preserving all 64 bits through display and focus.
User edits still apply the control domain before normalized host conversion.
Device-popover queries check selector support because REAPER's embedded WebKit
can disable popovers. An unavailable device panel must not interrupt rendering
or prevent the editor from acknowledging startup and becoming visible.
Worker failures are reported explicitly, independently of their status text.
Waiting and retrying states leave the view's error empty, so restoration retains
saved event arguments until their compiled schema arrives.
The browser bridge ends DAW parameter gestures when a control is removed,
its metadata changes, or another program is selected. Cancellation sends an
`endParamGesture` command without writing a value; ordinary host automation
updates retain ongoing gestures.
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
SPSC queue. Each callback consumes at most eight user events, bounding work even
while the message thread submits more. Onda's flat event-tensor metadata is
checked against the recursive schema plan when an engine is prepared. The native
borrowed-view entry point is not used because values cross both UI/audio threads
and the queue; the packed entry keeps ownership explicit and dispatch
allocation-free. The callback dispatches events at its next boundary without
allocation or locking; payload
rejection returns an empty execution-output batch and remains non-fatal, while
handler safety failures quarantine the engine. Structured delegate records are
decoded from the same schema into the runtime log. A build generation on every
command prevents stale event indices from crossing an engine replacement.
The bridge carries the generation of the rendered event schema as decimal text
to preserve all 64 bits. The processor rejects a stale generation before event
lookup or payload encoding, and the callback checks it again before dispatch.
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
for 200 ms before validating contents. Ancestor subscriptions also detect moves
and deletions, and symlink entries along the resolved path are watched alongside
their target inodes. On macOS, directory notifications compare only child-entry
identities on watched paths; file vnode notifications still invalidate in-place
edits even when their size and modification time are unchanged. When a linked path
disappears, the view reports the load error while the last valid engine and
checkpoint remain in use. Restoring the original path resumes loading.
The worker otherwise blocks on its condition
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
the queue releases its held notes. Adoption clears the old virtual holds; later
releases for those holds cannot dispatch note-offs to the replacement engine.
MIDI device selection remains host-owned.

The processor keeps host buffer capacity separate from the compiled Onda block
size. An instance preference of zero follows the host; a positive preference is
capped by the host capacity. The unloaded view can change it, and saved state
restores it before specialization. Host capacity changes resize scratch storage
without rebuilding an engine whose sample rate and Onda block size still match.
The timer prepares replacement dry-input storage in a one-slot handoff. Only
the audio thread adopts it, at callback entry, and returns the old storage for
message-thread destruction. Smaller callbacks can keep rendering during
preparation; a larger callback uses fallback until adequate storage is adopted.
Suspended host preparation can replace and reclaim scratch storage directly.

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
