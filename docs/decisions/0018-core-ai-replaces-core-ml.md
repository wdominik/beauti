# 0018: Core AI replaces Core ML; macOS 27 is the minimum

Status: Accepted. Date: 2026-10-27. Supersedes [0004](0004-apple-core-ml-default-and-cpu-skin-mask.md);
amends [0010](0010-two-languages-no-shell.md).

## Context

On Apple silicon both neural networks ran through ONNX Runtime's Core ML provider,
with the 256 px skin mask on the CPU because Core ML aborted on its pooling. Converted
unchanged, the models ran slower under Core AI than under Core ML. The cause was
placement, not the framework: the automatic compute-unit choice failed to load on the Neural Engine,
and the retouch U-Net's `align_corners=True` upsampling is not a resize the Neural
Engine supports, so the model alternated between the Neural Engine and the GPU.

With an explicit Neural Engine preference and that upsampling expressed as two
fixed matrix multiplications (the same interpolation), the retouch model runs on
the Neural Engine as fast as under Core ML and closer to the CPU reference. The
mask can then run on the GPU in parallel.

## Decision

- macOS uses Core AI for the retouch model (Neural Engine) and the skin mask (GPU)
  in both quality modes. Core ML is removed, including its Full Precision option;
  the device list offers Apple Acceleration and CPU.
- The minimum macOS version is 27.0. There is no fallback for older systems.
- ONNX Runtime remains on macOS for the CPU device, the face detector, and the eye
  landmarks.
- Core AI has only a Swift interface. A small Swift adapter (`src/backends/coreai.swift`)
  exposes it to C++ through a C interface; Swift is used nowhere else. This amends
  the two-language rule of 0010.
- The Core AI assets are produced from the existing models by
  `beauti_tools.models.prepare_coreai`, with the upsampling and pooling rewrites
  checked numerically against the original graphs.
- The render path receives a finished analysis after servicing readbacks, so a
  faster worker's results are not overwritten in the single result slot.

## Consequences

High Quality analysis drops from about 33 to 23 ms, and 4K fixture frame age from
133 to 100 ms at 30 processed frames per second; Performance mode is slightly
faster. The OBS render time rises by about 3 ms in High Quality because the mask
shares the GPU. The High Quality mask differs from the Core ML mask in about
0.005% of its labels; Performance maps pass the existing tolerances. A live-camera
review showed no visible difference.

Users on macOS 26 or earlier cannot use Beauti. Building needs the macOS 27 SDK
and Swift. Core AI assets are not byte-reproducible (the bytecode writer orders
operations differently per run), so the preparation step records their hashes
in `models/manifest.json`. Core AI is a 1.0 beta toolchain; conversion and
placement are rechecked on every Core AI or macOS update.

## Evidence

[Apple acceleration](../APPLE-ACCELERATION.md). M4 Pro, macOS 27.0.1, fixture runs without a camera.
