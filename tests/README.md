# Test contracts

Run the suite with `ctest --test-dir build -C Release --output-on-failure`.

`NewControlDefaults` reloads programs with newly declared controls before and
after selection seeding. Both products retain existing controls and parameters,
honor direct writes and queues received before compilation or adoption, and
preserve exact defaults through state restoration and subsequent reloads.
Repeating the untouched bank's `0.5` explicitly still counts as a host write.
Adoption callbacks are audited for allocations and locks. The case also runs in
the control-only configuration.
`UnloadedEndpointPrecision` and `UnloadedDefaultPrecision` check byte-exact
save/restore/resave of both banks while unloaded, restoring over an existing
engine, repeated unload, and subsequent loading with host values preserved.
They require exact values in initializers and rendered controls, and check that
newer identical float writes invalidate restored precision and scalar defaults.
Processor cases print their names before running. To isolate one scenario:

```sh
./build/onda_processor_tests IncompleteBufferStateRestore
./build/onda_processor_tests MessageThreadDelivery
```

Use `build/Release/onda_processor_tests.exe` for a Windows multi-config build.

## Coverage

| Test | Contract |
| --- | --- |
| `onda_example_tests` | Shipped examples compile and render finite, meaningful output. |
| `onda_plugin_tests` | SDK ownership, DSP and MIDI behavior, buffer decoding, portable checkpoints, project export, worker replacement, file watching, and compiler serialization. |
| `onda_processor_tests` | Host preparation, state persistence, defaults and automation, block-rate parameter smoothing, notifications, failure recovery, realtime callbacks, logs, scope/event adapters, editor lifecycle, and concurrent instances. |
| `onda_vst3_smoke` | Built bundles expose the expected metadata and parameters, instantiate independently, and render through JUCE's VST3 host. Bundle lookup also exercises the Windows layout on other platforms. |
| `onda_vst3_realtime_audit_tests` | Linux auditor startup, plugin loading, positive allocation/free/mutex detection, allocator semantics, callback budgets, and independent concurrent audit intervals. |
| `onda_vst3_realtime` | The exported VST3 behavior checks with Linux allocation and mutex interception enabled, after the auditor's own tests pass. |
| `onda_editor_bridge_tests` | The injected bridge ends cancelled parameter gestures without writing stale values, retains gestures across automation updates, and stamps events with the exact rendered generation. Registered when Node.js is available. |
| `onda_run_view_parameter_tests` | The actual run view and injected bridge preserve out-of-range and full-width integer defaults across publications and unedited focus/blur, retain focused drafts, and clamp actual edits. Registered when Node.js and Firefox are available. |
| VST3 validators | Steinberg's interface/protocol checks for both products. These complement the behavior checked by the smoke host. |

Run bridge tests directly with `node tests/EditorBridgeTests.mjs`. The
`EditorGestureCancellation` processor case checks the native command against
JUCE's actual value and gesture notifications, including cancellation after a
newer host automation value.
`StaleEditorEvents` checks that same-name events with a replacement schema
reject stale or invalid browser generations and accept the current generation.
`ClickableMidiKeyboard` also holds a virtual key across replacement, starts a
new host note, and checks that releasing the old virtual hold preserves it.
`HostBypass` checks note presses and releases, virtual-key cancellation, audible
output, scope output, and allocation/lock auditing for both products during
bypass. `BypassTransitions` checks 5 ms crossfades at 48 and 96 kHz, initial
bypass, reversals during a fade, empty and variable-size callbacks, MIDI splits,
and exclusion of control buses from dry output. The VST3 smoke host repeats
those transitions through the exported Bypass parameter. Controller conversion
tests check exact native discrete counts around JUCE's continuous sentinel and
saturation at VST3's signed 32-bit limit.
`ProjectBufferOverrideRestore` clears an override while the linked source is
broken, verifies that saved state and audio retain the complete checkpoint, and
checks that repairing the source restores the project default.
`AudioFileBuffers` also checks that clearing a required source binding after
image restoration still invalidates its checkpoint.
`RunViewAdapter` checks that waiting states preserve pending view restoration
while load failures still report an error.
`UnrangedParameters` checks plugin-owned 0–1 controls for scalar `f32`, `f64`,
`i32`, `i64`, and boolean parameters in both products, including editor smoothing,
discrete edits, automation, Reset, saved state, and native file reload after
adding a parameter while input controls are present. It verifies that Onda's
metadata remains unranged and authored defaults outside 0–1 survive loading,
Reset, and state restoration. It also checks adding an explicit range after an
unranged default and rejects saved defaults outside their scalar type.
`EventOnlyParameterSmoothing` checks that zero-frame MIDI and user-event
callbacks preserve subsequent `f32` and `f64` smoothing after startup and reset,
including partial first blocks, with allocation and lock auditing.
`InputControlEditor` checks input-control editor behavior, including a newer
editor write superseding a published automation curve before rendering. Both
products must ramp from the last rendered value across callbacks and MIDI
splits, without realtime allocations or locks.
`UiAndHostParameterChanges` checks the 30 ms editor default, direct host changes,
cancellation of editor ramps, state round-tripping, and allocation/lock auditing
during mixed editor and host changes. `BlockProcessing` checks unloaded-only
block-size selection, saved preferences and host limits, callback-delimited
blocks, curve interpolation at chunk boundaries, MIDI and queued user
events, oversized host callbacks, and allocation/lock auditing.
`CallbackProcessing` uses a shared reference for processor and exported VST3
tests with block sizes 1, 3, and 8, changing callback lengths (including zero),
parameter curves, sample-accurate control curves and buses, aliased input/output,
and MIDI that reads parameters at its offset. Persistent counters check both
block hooks exactly once per chunk and, with note state, verify that callback
boundaries preserve DSP state. `CallbackSmoothing` checks
actual elapsed samples for short and empty callbacks and MIDI splits with both
`f32` and `f64` parameters.
`ScratchHandoff` overlaps increasing
host bounds with timer-driven scratch preparation and reclamation, requiring
continuous small-buffer rendering, no realtime allocations or locks, and no
fixed-block engine rebuild. `AutomationDuringSeeding` keeps prepared defaults
isolated from host curves until acknowledgment, then checks retained endpoints,
host notifications, implicit curve reconstruction, selection changes, and newer
editor targets for both products. It also verifies edits before acknowledgment,
direct host writes that repeat a seed value, acknowledgment between capture and
processing, and edits after endpoint publication. `ControlAutomation` checks
constant-range host text conversion, exact double endpoints, and saving control
edits while seeding is pending. The VST3 smoke
host sends real queues to both products, checking curve values at the chosen
block rate without extra `block` executions. It also covers implicit
prior-callback endpoints, logarithmic `f64` values, discrete controls, constant
callbacks, and MIDI offsets within the block.
`onda_vst3_smoke` clears the audit preload and runs the functional checks by
themselves. On Linux, `onda_vst3_realtime` repeats those checks with a glibc
allocation/mutex auditor preloaded. Its measured VST3 callbacks must make no
allocator calls or non-null frees, and at most one `pthread_mutex_lock` call
for JUCE's existing callback mutex. This checks the exported wrapper path,
complementing the processor's audit.

CTest first runs `onda_vst3_realtime_audit_tests` as a required fixture. It
deliberately allocates and frees in separate intervals through every intercepted
heap entry point and all replaceable C++ allocation/deallocation forms. The
checks include aligned and page-aligned allocation, libc allocation helpers,
failed requests, alignment validation, `calloc` clearing, `realloc` preservation,
free's preservation of `errno`, disabled/reset intervals, and concurrent threads.
It also takes a mutex before shared-library constructors and loads an actual
plugin to exercise loader TLS updates. A failing auditor test prevents an
instrumented plugin verdict; the ordinary smoke test remains independent.

The processor's `RealtimeAuditInstrumentation` case uses the same allocation
fixtures to verify its linker wrappers and C++ hooks before testing callbacks.
Allocator counters record entry-point calls, including failed attempts; they
are not outstanding-allocation counts. These audits cover the documented heap
and mutex interfaces on the measured threads. They do not measure custom memory
mapping, other synchronization interfaces, I/O, or execution deadlines.
The preloaded auditor follows [glibc's allocator interposition requirements](https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html), including its initial-exec TLS requirement.

## Scheduling

State semantics are distinct from scheduling. `test::service(processor)` calls
the processor's actual timer callback on the message thread. Tests can therefore
check a pending dirty-state notification, log drain, or parameter seed without
requiring a 20 Hz timer to fire within 50–100 ms. It does not replace the worker,
compiler, DSP, or notification implementation.

`MessageThreadDelivery` deliberately does **not** use that helper. It checks that
the real JUCE timer delivers project changes, parameter defaults, and logs through
the native message loop, with a deadline. Native WebView2 readiness and asynchronous
export completion also wait for their actual completion signals.

For every state-notification assertion:

- Complete the relevant build/adoption when it is part of the operation, then
  service pending updates before recording the baseline for the next operation.
- For an incomplete selection, check notification of the persisted selection
  without requiring a successful build or host preparation.
- For restore and identical recompilation, complete preparation and explicitly
  service updates before asserting that the notification count is unchanged.
- Do not assert an exact count of asynchronous notifications: changes may coalesce.
  Synchronous editor preference changes can be checked immediately.

Publication, audio adoption, and message-thread seeding are separate stages.
`waitForPublishedReplacement` intentionally yields only to the worker; pumping
messages there would invalidate the tests of unadopted or unseeded replacements.
Host-preparation and offline-render tests also retain their no-message-loop contract.

Asynchronous waits use a steady-clock deadline rather than an assumed number of
timer ticks. Worker gates have bounded waits, and CTest imposes a 180-second process
timeout, including teardown. The remaining 600 ms watcher observations and 250 ms
compiler-overlap observation are explicitly integration checks over finite windows;
they are not scheduler-independent proofs that an event can never happen.

Watcher rebuild counts are measured from the first published engine. Initial
compilation may retry after discovering imports or canonical path aliases; it is
not required to take exactly one compiler call. Both standalone and imported-source
fixtures require no additional compilation for a timestamp-only edit and exactly
one additional compilation for each tested content edit, including equal-size
edits that preserve the modification time. Pinned-initializer coverage checks
that disk preparation and saved-image fallback each capture host values once.
Pending-seed regressions cover explicit reloads, native source edits, host
re-specialization, and acknowledgment during a gated build. Project-default cases
render all five Onda primitive types through disk loading, filesystem-free image
restoration, and export, and reject non-`f32` audio-file overrides.
Filesystem-project regressions combine lowercase and mixed-case extensions,
contained and external imports, and manifest-relative buffer assets; each
checkpoint must render after deleting its files and exporting it again.
Ancestor-move coverage requires a missing-path error while the old engine keeps
rendering, then restores the original path and checks recovery. Native watcher
coverage also rejects unrelated ancestor and same-parent sibling activity and
exercises Linux directory symlinks that share an inode with an ancestor
subscription. POSIX buffer cases remove, restore, and atomically retarget direct
and indirect directory symlinks, checking retained checkpoints and replacement
audio.

Control-input regressions share one fixture between the processor and exported
VST3s. They cover default-disabled mono bus metadata, individual and combined
activation, fixed channel positions with an earlier bus disabled, disconnected
scalar defaults, per-sample clamping including NaN, MIDI timing, short final
blocks, and bypass using only main audio inputs. Processor
renders audit allocations and locks. Preparation rejects control array defaults
that the pinned SDK cannot expose, and rejects scalar/array `f64`, `i32`, and
`bool` control ports. An additional fixture enables every control bus while the
program declares none, supplying NaNs on those buses and checking that output
stays valid. Both processor and exported VST3 tests route the last control bus
with all earlier control buses disabled, then check its current parameter value
after disconnecting it. Processor tests automate every lane concurrently across
logical blocks and require a 1,024-point queue for an undeclared control to read
only its endpoint, retaining that value when a reload declares the control.
Non-default main/output configurations run
the `ControlInputs` processor case alongside the VST3 smoke test and validators.

## Platform assumptions

- Every test process owns a canonical temporary root containing spaces and Unicode.
  Convert filesystem paths through `pathToJuce` at JUCE boundaries; native narrow
  `.string()` is not a UTF-8 conversion on Windows.
- Portable project-image paths use `/`, independent of native filesystem separators.
- Floating-point checks reject NaN and infinity. Audio-file expectations account
  for the fixture's FLAC quantization rather than requiring bit-identical floats.
- Aligned-allocation tests cover small and over-aligned requests and zero sizes.
  C++ allocation/deallocation auditing runs on all platforms; C allocation/free and mutex-lock
  interception use Linux linker wrapping. Passing on macOS or Windows does not
  establish the same interception coverage.
- Editor lifecycle tests do not establish that every native browser renders the UI.
  The desktop WebView2 handshake is checked on Windows; macOS and Linux exercise
  editor ownership, dimensions, preferences, and automation without requiring that
  browser handshake. VST3 validation does not replace testing in a real DAW.

Linux success cannot establish macOS or Windows success. The release workflow runs
the suite independently on all three platforms.

`ControlAutomation` compares continuous and discrete cursor values and rendered
samples against an independent reference that scans points afresh per sample
and interpolates with a `long double` weighted sum. Deterministic cases and
2,000 reproducible random curves cover implicit sample -1 endpoints, duplicate
offsets, jumps, stationary spans, empty queues, invalid offsets, NaN/infinity,
normalized clamping, and callbacks of zero through 128 samples. Split and whole
renders must also match exactly. It then checks every processor output sample
with Onda block sizes 8, 3, and 1. It covers range conversion, names, MIDI timing,
bypass, routed bus precedence, seed acknowledgement, saved pending defaults,
and state restoration, with allocation and lock auditing on the render path.
The exported VST3 smoke test repeats sample-level curves and MIDI timing for
both products, checks parameter and bus titles, and flushes control endpoints
with a zero-sample `ProcessData` call.
It also waits for real timer-driven controller updates, requires no host edit
or gesture feedback from incoming automation, and verifies that the next JUCE
hosted callback retains an exact `f64` endpoint. Pending control reloads change
defaults and ranges before seed acknowledgment and require the inherited
normalized values both before and after acknowledgment.
Offline VST3 restoration runs on the processing thread and requires no outgoing
parameter queues from incoming slot or control automation. It then waits for
message-thread controller delivery and checks that a hosted callback retains
the exact `f64` endpoint. Routed control automation reads only its endpoint;
disconnecting the bus resumes that value without rendering discarded curves.

`ControlDefaultPrecision` checks exact scalar defaults in wide ranges before
and after seed acknowledgment, save/restore, source reloads, Reset, and a Reset
overlapping a gated build. It also cancels a borrowed curve with Reset after
publication of a replacement default with the same normalized float, requiring
both callbacks to render the committed scalar. A concurrent Reset-cache audit
rejects mixed scalar/type/range/stamp snapshots, including full-width integer
payloads. Identical float host writes and queue
endpoints must still supersede those defaults. `ControlAutomation` also checks
asymmetric wide ranges in bulk fills and linear spans, requiring identical
samples across segment splits and exact endpoints.
It also checks wide bipolar zero crossings against normalized interpolation
through both products, MIDI splits, logical blocks, and host callback boundaries.
`ControlRendering` checks exact floating-point bits against scalar ramp advancement
and `std::lerp` curve sampling. It covers retargets, duration changes, empty
renders, positions beyond double's exact integer range, signed zero, subnormal
and extreme endpoints, constant spans, and identical whole/split rendering.
`UnrangedParameters` covers full-width `i64` defaults on both sides of the exact
double integer range and at the signed 64-bit limits. It checks declaration-order
slots, initialization, normalized automation and editor writes, Reset, reloads
with changed authored defaults, and saved-state round trips, with realtime
allocation and lock auditing on rendering.
`SeedNotificationThreads` runs offline processing
and host preparation on a separate thread, then requires slot seed listeners to
wait for message-thread delivery. It also requires host default-cache refreshes
to wait for that delivery. `ParameterDefaultNotifications` checks coalesced
refreshes after selection seeds and newly declared controls, and no refresh for
incoming automation or reloads retaining existing values. The exported VST3
controller checks plain/normalized conversions for logarithmic, bipolar,
integer, boolean, unmapped and constant-range controls; incoming automation
must not generate controller edits or host value-cache refreshes.
`AutomationEchoWork` requires dense stationary
queues after Reset to inspect only one endpoint for controls, settled slots, and
unmapped slots. These cases run in both default and control-only configurations.

`AutomationAfterUnload` and `AutomationFlushAfterUnload` deliver slot and control
queues after unloading an engine with an acknowledged or pending default seed.
They require immediate endpoint commits, discard endpoints retained by the old
seed, and preserve the latest values through timer delivery, an empty callback,
state restoration, and reloading. Parameter-only flushes deliberately skip DSP.
Both cases audit realtime allocations and locks and run for both products in
default and control-only configurations.

`AutomationFlushAfterFault` faults the first render of an engine with pending or
acknowledged defaults, then delivers parameter-only slot and control queues
before another audio callback can retire it. Endpoints must commit immediately,
retain double precision through timer delivery and state restoration, and add
no realtime allocations or locks. It runs for both products in the default and
control-only configurations.

`ParameterEndpointPrecision` checks concurrent stamped snapshots of exact host
endpoints and requires reloaded `f64` initializers to receive the same value as
DSP. `ControlEndpointPrecision` retains precise normalized endpoints through
reload and saved-state restoration, then verifies that a newer float write
supersedes them. It also cancels a published control curve with Reset's write
and requires the exact scalar default in that callback and the following one,
with realtime allocation and lock auditing. Both cases run for both products
in the default and control-only configurations.

`ParameterDefaultPrecision` checks exact ordinary `f32` and `f64` defaults in
wide ranges through selection, Reset, source reloads, initializer snapshots,
pending-seed acknowledgment, and saved-state restoration. Identical float host
writes and queue endpoints must supersede those defaults; an editor write with
the same normalized value must still start its plain-unit ramp. A gated build
checks that Reset preserves the default it actually wrote when the replacement
declares a different default. A logarithmic `f64` default is compared against a
pinned initializer to detect even small round-trip errors. The exported VST3
smoke test repeats exact-default restoration and invalidation by a matching
float endpoint for both products, including a full-width `i64` default and its
initializer. Rendering audits allocations and locks.
The case also reloads between seed commits and whole-bank acknowledgment,
requiring exact snapshots from both banks and the correct replacement
initializer. This regression runs in the default and control-only builds.
