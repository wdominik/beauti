# 0004: Apple default is regular Core ML; the Performance skin mask runs on the CPU

Status: Superseded by [0018](0018-core-ai-replaces-core-ml.md). Date: 2026-10-25 (native M4 Pro measurements).

## Context

macOS offers Apple Acceleration (Core ML with low-precision GPU accumulation)
and Apple Acceleration (Full Precision). In Performance quality the skin mask
graph is the 256 px FP32 variant. The Windows work found Full Precision faster
on NVIDIA ([0003](0003-nvidia-full-precision-default.md)), which raised the
question whether the Apple default should follow, and whether the mask could
move onto Core ML.

## Decision

- The Apple default stays regular Core ML: MLProgram format, compute units
  `ALL`, static input shapes, model cache, low-precision accumulation on. The
  default applies when the bundled runtime registers the Core ML provider
  ([0015](0015-default-device-follows-the-machine.md)).
- In Performance quality the 256 px skin mask runs on the CPU provider; the
  retouch graph runs on Core ML. High Quality runs both on Core ML.
- The workaround is confined to Apple backends in `model_plan`; the NVIDIA
  provider uses the same FP32 mask graph on the GPU.

## Consequences

- Full Precision on Apple measured a median fast Retouch of about 18.5 ms
  versus 10.2 ms for regular Core ML; Windows findings do not transfer.
- Compute-unit variants (CPU+GPU, CPU+Neural Engine) and earlier readback
  dispatch were measured and rejected: either slower or fewer distinct
  processed frames (down to 17.9 FPS from about 30 on 4K).
- The CPU placement of the 256 px mask predates this repository; the code calls
  it a verified MPS workaround, and the native comparison kept it. Revisiting it
  requires an isolated native comparison with proven output correctness and
  operator placement. Re-tested on macOS 27.0.1 with ONNX Runtime 1.31.0: the
  256 px graph aborts the process in MPSGraph when placed on Core ML, while every
  model that does run on Core ML is a single partition.

## Evidence

- Measured during development before the first release; the figures in this record are the summary. The raw measurements are not part of the repository.
- `src/backends/backend.cpp`, `model_plan` and `configure_provider`.
