# 0005: Mask erosion and feathering run in the OBS shader for every provider

Status: Accepted. Date: 2026-10-25.

## Context

The skin mask leaves the engine as a raw float map. Erosion (five-pixel
ellipse), separable 17-tap sigma-2 Gaussian feathering, hard exclusion, and
confidence weighting were CPU post-processing in the worker. Profiling
showed this CPU span as the largest remaining cost in High Quality.
Alternatives were CUDA interop or provider-specific kernels.

## Decision

The engine returns raw maps; `SkinGPU` prepares them with three shared shader
passes (`MaskInset`, `MaskFeatherH`, `MaskFeatherV`) on the OBS graphics
abstraction, identically for every inference provider. Prepared
reference maps remain supported through an explicit format flag.

## Consequences

- More processed frames in both quality modes, with lower frame age.
- Three reusable float render targets, about 3 MiB at 256 px and 12 MiB at
  512 px. Preparation repeats on slider-only recomposition; caching it is a
  possible later optimization, not part of the decision.
- Old/new comparison over 180 outputs: at most one byte level RGB difference,
  exact alpha, protected pixels, and bypass endpoints.
- No CUDA interop, no platform-specific implementation, no new threads: the
  change stays within the complexity policy, unlike the rejected cuDNN
  experiment.

## Evidence

Measured during development before the first release; the figures in this record are the summary. The raw measurements are not part of the repository. Confirmed natively on Windows (Direct3D 11) and macOS.
