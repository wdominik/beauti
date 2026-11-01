# 0012: The engine has one entry point; the CPU references live in a separate library

Status: Accepted. Date: 2026-10-25.

## Context

`Engine::process()` reimplemented the GPU compositing pipeline on the CPU
(blend, edge weight, mask feathering, Texture through the effects library). It
was used only by `beauti-bench` and a smoke test, and it had to be kept in step
with the shader by hand. The numerical tests already compare the shader against
`src/effects/`, a separate portable reference library.

## Decision

`Engine::analyze()` is the single entry point and returns maps (retouch blend,
raw skin mask) plus the face box and timings. The CPU compositing path is
removed. `beauti-bench` returns exactly what the plugin consumes, in a documented
binary protocol mirrored in Python. `src/effects/` stays a separate shared
library for the numerical tests and is not linked into the engine.

`Frame::used_backend` reports the `beauti::Backend` value itself, so there is
one numbering; saved OBS settings use the same values.

## Consequences

- The engine shrank by about a quarter; there is one pipeline to verify.
- Tests that compared libobs output against the removed CPU composite
  (`bench_gpu.py`) are replaced by `beauti-gpu-check` plus
  `check_finishing_pixels.py`, which reconstruct feathering with an independent
  OpenCV reference.
- Worker comparisons (`compare_workers.py --exact`) compare boxes and maps
  texel by texel between two bundles.

## Evidence

- `CHANGELOG.md`, *Removed* and *Changed*.
- `tools/benchmarks/worker.cpp` (protocol header),
  `tools/beauti_tools/bench/protocol.py`.
