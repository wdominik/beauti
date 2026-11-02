# Performance and image-quality acceptance

The product goal is a convincing, temporally stable real-time image, not bitwise
identity across all processors. Small image differences are acceptable candidates
when they buy a substantial measured performance benefit. This is not permission
to ignore regressions or to assume an unmeasured speedup. No release is to be
prepared until explicitly requested.

The shared [output continuity contract](OUTPUT-CONTINUITY.md) requires bounded
last-good presentation across temporary inference delays on CPU, NVIDIA, and
Apple. Original flashes caused by ordinary scheduling jitter are regressions;
repeated processed frames must remain distinct from throughput improvements.

## Complexity and cross-platform policy

Prefer clean, maintainable code and established runtime defaults when measured
performance gains are negligible or inconclusive. A technically faster candidate
is not automatically an improvement: weigh repeatability, latency, visual
quality, extra resource ownership, failure paths, and ongoing test burden against
the benefit. Retain the simpler baseline when the tradeoff is not worthwhile.

Prefer shared processing and the existing OBS graphics abstraction across Apple,
NVIDIA, and CPU-inference modes. Keep backend-specific alternatives small and
document why they are needed. Record each experiment's outcome and relevant
Apple follow-up, including rejected changes, without treating Windows validation
as native Apple evidence. Do not introduce GPU-model-specific optimization rules.

## Keep two distinct types of comparison

**Implementation-preserving changes:** buffer reuse, fused loops, and equivalent
vector arithmetic should retain the original image. Same-backend old/new exact
comparisons are useful bug detectors for channel swaps, alpha mistakes, padding,
rounding, and stale buffers. Keep those tests strict where equivalence is intended.
The recent 72-frame suites compare each backend to its own previous build; they
do not require CPU, NVIDIA providers, and Core AI to produce identical pixels to each other.
Existing cross-provider and shader-reference checks already allow numerical
differences.

**Intentional approximations:** precision changes, alternate resampling, reduced
working resolution, or reduced analysis cadence can legitimately change pixels.
Evaluate them with a separate quality/performance report rather than treating an
exact-match failure as an automatic rejection or silently weakening all tests.
Current `compare_workers.py` tolerances are narrow engineering regression limits,
not a complete perceptual acceptance policy.

Retain exact structural invariants where the operation promises them: zero-effect
bypass, alpha preservation, stage order, valid output, and unchanged explicitly
protected/outside-effect pixels. Record intentional changes to detection regions
or mask boundaries separately; do not hide them in a full-frame average.

## Candidates using the existing models

CPU mode uses ORT automatic thread sizing ([decision 0016](decisions/0016-cpu-inference-thread-sizing.md));
CPU sessions next to an accelerator use the auxiliary budget
([decision 0022](decisions/0022-tensorrt-segmenter-and-auxiliary-threads.md)). Most
intermediate fields use half floats ([decision 0025](decisions/0025-resource-use.md)).
Reduced-resolution large-scale finishing statistics remain a separate, unimplemented
candidate; measure graphics-pass cost first.

A shared color cache between the finishing passes was measured and rejected: it
did not pay for the extra graphics target and pass.

| Candidate | Potential benefit and important limit |
| --- | --- |
| Existing FP16 modes | No FP16 measurement exists for TensorRT RTX; keep the FP32-model default until a comparison justifies changing it. Apple runs float16 assets on the Neural Engine and GPU ([Apple acceleration](APPLE-ACCELERATION.md)). |
| Lower analysis resolution | May reduce capture/readback and preparation work. Current model input sizes remain fixed, so it does not proportionally reduce model inference cost or all full-resolution finishing work. |
| Less frequent detection with tracking/reuse | May avoid detector work while processing current frames. Requires fast-motion, occlusion, new/lost-face, and recovery tests; stale masks or edge lag may be more objectionable than small numeric differences. Report detector/model update cadence separately from rendered FPS. |
| Lower-resolution or approximate finishing fields | May reduce selected GPU costs but can alter texture, face edges, and local tone/lightness. Retain full-resolution output and the four-stage order; validate temporal stability and texture preservation. |

TensorRT RTX runs the existing model files with the provider's arithmetic
defaults. "Full Precision" identifies FP32 model selection, not bit-exact
arithmetic. There is one NVIDIA policy, with no per-card optimization table.
The skin segmenter runs on the same device
([decision 0022](decisions/0022-tensorrt-segmenter-and-auxiliary-threads.md)).

## Measure the tradeoff explicitly

- Compare one change at a time against the same fixed input sequence, aligned by
  source frame. Measure distinct processed FPS, p95 frame age, startup, and
  sustained behavior; repetitions or older analysis must not masquerade as speed.
- Measure RGB differences in the active face region, their distribution and
  spatial extent, perceptual color/structure differences, and mask alignment.
  Include close crops of eyes, lips, hair boundaries, and skin texture. A single
  maximum byte difference or global average is insufficient.
- Evaluate motion: flicker, unstable color, shimmering texture, halos, ghosting,
  delayed masks, and abrupt recovery. Use varied faces, skin tones, lighting,
  head poses, and motion rather than accepting a single still fixture as visual
  proof. The user retains the final camera/OBS assessment.
- Require a repeatable, worthwhile gain. A working prioritization target is about
  20% more processed throughput or reaching sustained 30 processed FPS from the
  current approximately 25.6 FPS while preserving latency and perceived quality.
  This is a proposed engineering target, not a user-specified numerical threshold
  or a performance promise. A few percent does not justify visible artifacts.

Do not alter existing passing reference outputs or blanket-relax correctness
checks merely to make an approximation pass. Record the quality tradeoff, retain
the reference path for comparison, and adopt only measured improvements. Apply
the same principles to the [Apple optimization options](APPLE-ACCELERATION.md#optimization-options-not-yet-adopted).
