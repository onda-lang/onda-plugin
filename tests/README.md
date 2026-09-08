# Test contracts

Run the suite with `ctest --test-dir build -C Release --output-on-failure`.
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
| `onda_processor_tests` | Host preparation, state persistence, defaults and automation, notifications, failure recovery, realtime callbacks, logs, scope/event adapters, editor lifecycle, and concurrent instances. |
| `onda_vst3_smoke` | Built bundles expose the expected metadata and parameters, instantiate independently, and render through JUCE's VST3 host. Bundle lookup also exercises the Windows layout on other platforms. |
| VST3 validators | Steinberg's interface/protocol checks for both products. These complement the behavior checked by the smoke host. |

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

## Platform assumptions

- Every test process owns a canonical temporary root containing spaces and Unicode.
  Convert filesystem paths through `pathToJuce` at JUCE boundaries; native narrow
  `.string()` is not a UTF-8 conversion on Windows.
- Portable project-image paths use `/`, independent of native filesystem separators.
- Floating-point checks reject NaN and infinity. Audio-file expectations account
  for the fixture's FLAC quantization rather than requiring bit-identical floats.
- Aligned-allocation tests cover small and over-aligned requests and zero sizes.
  C++ allocation auditing runs on all platforms; C allocation/free and mutex-lock
  interception use Linux linker wrapping. Passing on macOS or Windows does not
  establish the same interception coverage.
- Editor lifecycle tests do not establish that every native browser renders the UI.
  The desktop WebView2 handshake is checked on Windows; macOS and Linux exercise
  editor ownership, dimensions, preferences, and automation without requiring that
  browser handshake. VST3 validation does not replace testing in a real DAW.

Linux success cannot establish macOS or Windows success. The release workflow runs
the suite independently on all three platforms.
