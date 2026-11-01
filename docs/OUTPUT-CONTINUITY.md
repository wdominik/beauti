# Output continuity contract

Status: implemented in the shared development source, with automated policy and
Windows and macOS integration tests. This contract
applies equally to CPU, NVIDIA TensorRT RTX, and Apple Core AI, in both quality modes.
No GPU-model-specific exceptions. No release is authorized by this document.

## Primary requirement

Once a compatible processed frame is available and effects remain enabled, a
temporary lack of a newer result must not, by itself, cause a flash of the
unprocessed live source. Preserve visual continuity across normal inference
jitter, queue replacement, and short stalls. Treat processed throughput, result
freshness, and effect continuity as separate metrics.

Absolute continuity, an always-current picture, and bounded latency cannot all
be guaranteed if processing stops or is slower than the source indefinitely.
Prefer a short repeat of the complete processed picture over a one-frame original
flash. Bound the repeat duration; do not freeze indefinitely or call repeated
frames increased processing throughput. Camera/audio synchronization remains part
of final live validation.

## Shared behavior

1. Keep the last complete, compatible processed snapshot and its matching maps
   until a complete successor is ready. Publish and swap as one coherent result.
   Own the displayed resources independently of pending jobs and readback slots.
2. Separate a freshness target from a hard stall limit. Crossing a soft age target
   is a diagnostic/scheduling event, not automatic permission to show the original.
   Budget for readback, queue waiting, processing, render delivery, and normal
   jitter. Use bounded recent completion intervals/runtime observations, with
   conservative startup behavior, rather than an arbitrary GPU-only 200 ms cutoff.
   The same algorithm applies to every backend; measured timing can differ.
   Let `interval_ms = max(last_good_analysis_ms, 1000 / requested_analysis_fps,
   observed_delivery_ms)`, where `observed_delivery_ms` is the time between the
   last two uninterrupted good results (capped at the hold maximum, forgotten after
   a cadence change or a fallback). It covers scheduling delay between inference
   and delivery, which the engine time cannot see.
   The implemented last-good window is `clamp(2 * interval_ms + 200, 350, 2500)`
   milliseconds from acceptance, additionally capped at five seconds from capture.
3. Track both absolute source-frame age and time since actual successful progress.
   Repeating/recomposing an old snapshot must not reset either freshness or worker
   health. Use a monotonic clock and explicit bounds so a hung worker cannot
   extend its own hold window indefinitely. A 150 ms transition may follow the
   hold limit; maximum retained-image contribution is therefore 5.15 seconds
   from capture, not an unlimited freeze. These bounds do not guarantee continuity
   on hardware whose normal processing time exceeds the configured caps.
4. Keep only the latest pending input and discard obsolete queued work. An older
   displayed result may remain valid while a newer result is still in flight.
   Reject out-of-order or incompatible results; do not let them replace the last
   good display. Strength changes retain valid analysis and the existing ramps.
5. Never apply old face maps to an unrelated newer image just to maintain motion.
   Repeating the aligned snapshot is the first implementation. Tracking/warping
   onto current frames is a separate approximation requiring independent motion,
   identity, occlusion, and recovery validation.

## Distinguish transition reasons

| Situation | Required behavior |
| --- | --- |
| Startup, before any usable result | Original output is allowed; report that processing is starting. Enter processed output when a valid result arrives. |
| Late/in-flight result, within bounded hold policy | Repeat the last compatible processed snapshot. Do not toggle into live-original output. |
| Persistent overload with continuing progress | Maintain bounded continuity, report actual throughput/age, and reduce avoidable work. Any automatic quality/resolution change needs a separate documented policy; do not silently override user settings. |
| Hard stall, persistent inference error, or graphics failure | Enter an explicit degraded state. Use a controlled transition to live original when compatible resources allow it; an immediate fallback may be necessary after resource loss. Recover only on a valid complete result, with hysteresis to prevent repeated processed/original toggling. |
| Completed no-face result | Bridge one completed miss after a valid face, bounded by `clamp(1.25 * interval_ms + 75, 150, 1500)` ms from that miss and the existing last-good deadline. A second consecutive miss ends the hold immediately. Neither miss extends the original deadline. Do not keep a previous person's mask on a different image or retain an old face indefinitely. |
| All effects intentionally set to zero or filter disabled | Honor explicit bypass. Do not keep showing the previous effect in the name of continuity. Preserve exact zero-effect endpoints and existing strength ramps. |
| Source/scene discontinuity, incompatible dimensions or color space | Invalidate incompatible history. Do not carry another scene/person's processed image across a source change. Backend/quality changes may preserve a compatible display while reinitializing only when the compatibility rules explicitly allow it. |

Every fallback must have an observable reason. Distinguish startup, explicit
bypass, confirmed no-face, source invalidation, hard stall, and resource failure
from an avoidable scheduling-induced original flash. Smooth fallback transitions
are not a substitute for keeping valid processed output during ordinary jitter.

The first implementation crossfades complete snapshots to/from the live source
over 150 ms. It does not warp stale masks onto a new frame. After a hard stall,
confirmed no-face, no-face timeout, or resource failure, two consecutive valid
face results are required for recovery; a worker error interrupts that sequence.
Normal startup requires one valid result. Existing strength ramps retain exact
zero endpoints, then explicit all-zero switches to live bypass.

Changing the requested analysis frequency recomputes the budget from the original
capture/completion timestamps, never from the current render tick. This permits
intentional 1 Hz analysis without diagnosing normal waiting as a stall. It does
not resurrect an already expired state or reset the absolute age cap. The fixed
150 ms first-miss prototype was rejected after the CPU fixture showed roughly
800 ms analysis: its next result could not arrive within that grace period.

A render-context generation rejects results from incompatible source, dimensions,
color-space, or settings contexts. A conservative 16 x 16 RGB readback comparison
also invalidates history on large scene changes (mean absolute channel change
above 70/255 and over 180 of 256 samples changing by more than 80/255 on average).
This is a best-effort scene-cut signal with readback latency, not identity tracking
or guaranteed recognition of subtle cuts. Large lighting changes may also reset
history. Immediate incompatible-context invalidation does not crossfade the old
scene. Validate these limits with native camera motion and scene switching.

`BEAUTI_TIMING=1` reports presentation reason changes. The separately built
`beauti-continuity-test` module adds fault injection; it is not installed or staged
by the production build. Production `beauti` does not contain those injection hooks.

## Automated acceptance

Use deterministic delay/error injection in test harnesses, outside normal
production behavior. Assert observable output frames, not only internal counters.

- Slow completions, isolated spikes beyond 200 ms, replaced queued jobs, and
  variable completion intervals: zero scheduling-induced original flashes after
  warmup while within the declared hold window; bounded memory and queues.
- Repeated output must preserve source-frame/map alignment. Record new processed
  frames, repeated frames, age percentiles, longest repeat, and fallback reasons
  separately. Do not count hold repeats as fresh results.
- Persistent worker stall/error: the hard limit expires despite ongoing render
  ticks and strength changes; fallback and recovery do not oscillate.
- Face disappearance/reappearance, occlusion, scene cuts, resize, source changes,
  backend/quality changes, and output-resource failure: no incompatible history,
  wrong-face mask, stale publication, or indefinite hold.
- Positive slider dragging and all-zero transitions: no accidental original
  flashes while effects remain active, and exact intended bypass endpoints.

Run shared policy tests independently of acceleration hardware. Validate actual
rendering/inference integration on Windows CPU/TensorRT RTX and native Apple CPU/Core AI;
Windows evidence does not certify Apple. Final camera/OBS judgment remains with
the user.

## Implementation and remaining validation

A fixed source-age cutoff (200 ms from capture) drops an already
displayed result before its successor arrives; the hold window above is derived
from observed intervals for that reason. `src/core/continuity.h` supplies the one
provider-independent policy; render-thread ownership and blending are integrated
in `src/plugin/filter.cpp`.

CTest covers deadline boundaries, isolated/repeated misses, recovery, invalid
timestamps, out-of-order results, reset, and resource failure. The isolated
`tests/obs/check_continuity.py` exercises missed detections, delayed completion,
hard stall, inference error, recovery, and a conspicuous scene cut on actual OBS
output. Its report preserves repeated source samples separately from fresh work;
source counters during crossfades are not reliable throughput/age measurements.
Continue performance work independently; a continuity improvement
must not be reported as an inference-speed improvement. Document Apple integration
in [Apple acceleration](APPLE-ACCELERATION.md) and apply the
[performance acceptance criteria](PERFORMANCE-ACCEPTANCE.md).

## Validation

All six injected faults pass with and without Eyes on macOS (Core AI, also under
ThreadSanitizer) and on Windows (TensorRT RTX and CPU). The normal and injected
modules are separate; no injected module is installed. Live-camera/audio
synchronization, subtle scene cuts, and long-session behavior remain unverified.
