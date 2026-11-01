# 0017: Direct TensorRT RTX through ONNX Runtime

Status: Implemented for development; camera acceptance pending. The 256 px mask pooling exception lapsed with [0021](0021-mediapipe-detection-and-segmentation.md). Date: 2026-10-26.

## Context

Use a small NVIDIA deployment on Windows while retaining the shared engine, CPU mode, eight prepared models, and OBS pipeline.
Windows ML was considered, then explicitly deferred. No model export, training,
new model, tracking change, or GPU-model-specific policy accompanies this change.

## Decision

- Use ONNX Runtime 1.31.0's CPU distribution with NVIDIA's standalone TensorRT
  RTX EP 0.5.0 cu13 and matching TensorRT RTX 1.7 libraries. Pin the official
  archive, SHA-256, and four DLL names, sizes, and hashes in `buildspec.json`. Do not
  fork the EP. Packages do not contain these DLLs: users copy them from NVIDIA's
  archive next to the plugin ([installation guide](../INSTALL.md)), and the plugin
  checks their sizes before loading them. Without them new filters use the CPU.
- Register through the official environment/device API. Share one synchronized
  NVIDIA registration for the module lifetime. RAII preserves session/environment/
  library ownership order. CPU engines do not load NVIDIA libraries. Load absolute
  plugin-folder paths with restricted search flags; do not alter OBS's DLL search policy.
- Preserve settings IDs 3/4 and the Full Precision default (FP32 model files).
  FP16 remains explicit. Use standard CUDA graphs and local runtime caching on
  every supported NVIDIA GPU; retain other provider defaults. The precision label
  describes model files, not a bit-exact arithmetic promise.
- Require a compatible RTX GPU (EP targets SM 8.6, 8.9, or 12.0+) and a CUDA
  13-capable driver. Default machines without one use CPU; explicit unsupported
  GPU selections report errors. No CUDA EP, cuDNN, cuBLAS, NVRTC, Windows ML,
  Windows App SDK, or toolkit is needed.
- Keep every model byte and dimension. The 256 px Performance skin mask has four
  oversized AveragePool nodes that this parser cannot handle. Only for this model,
  use the documented `nv_op_types_to_exclude` option to run AveragePool on CPU.
  The remaining graph runs on TensorRT. Profiling rejects unexpected CPU operators
  or whole-model fallback. High Quality needs no exclusion. Remove the exception
  when an upstream update passes the same validation without it.
- Cache locally/offline, using model, ORT, and pinned provider DLL identities in the
  bundle key plus the provider's runtime/device compatibility checks. Do not ship
  GPU-specific caches or add a custom engine serialization layer.

## Trade-offs and evidence

Initial compilation takes time; warm caches reduce startup. Strict cross-provider map
comparisons remain failed evidence, not relaxed tests. Final-pixel differences require
camera review before release.

The EP's published matrix lists ORT 1.25-1.30. Local 1.31 validation does not extend
NVIDIA's compatibility guarantee. Maintain that test on upgrades.

## Earlier decisions and Apple follow-up

The single NVIDIA policy, saved selections, CPU
detector, four-stage order, and common continuity contract remain in force.

Apple stays on Core ML. Run native Apple build/TSan and CPU exactness checks for
shared environment ownership. Investigate narrowly partitioning unsupported
pooling on Apple only if the supported Core ML API permits it and measurements
justify replacing the whole-mask CPU fallback. Do not copy NVIDIA-specific options
or change models/cadence. The rejected color-field-cache work stays closed.

## Primary sources

- [Integration](https://nvidia.github.io/TensorRT-RTX-EP-ABI/integration-guide.html)
- [Release/matrix](https://github.com/NVIDIA/TensorRT-RTX-EP-ABI/tree/v0.5.0)
- [Options](https://github.com/NVIDIA/TensorRT-RTX-EP-ABI/blob/v0.5.0/src/tensorrt_rtx_provider_options.h)
- [Prerequisites](https://nvidia.github.io/TensorRT-RTX-EP-ABI/prerequisites.html)
