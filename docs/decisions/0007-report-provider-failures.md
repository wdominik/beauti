# 0007: Provider failures are reported, never silently replaced by CPU

Status: Accepted. Date: 2026-10-24.

## Context

When the selected processing device cannot be initialized (missing provider
libraries, an unsupported GPU, a Core ML compilation failure), a plugin could
fall back to the CPU provider and keep running. The user would then see a slow
filter without knowing why.

## Decision

`configure_provider` throws when the requested provider is unavailable; the
worker reports the error in the status line and the log. The session is not
switched to CPU. A saved selection that the current build does not support
(for example an NVIDIA choice loaded by the CPU build) normalizes to the build's
default at settings time, which is visible in the Processing Device list.

Which device a new filter starts on is a separate rule: when the machine lacks
the build's accelerator, the default is CPU and the status line says so
([0015](0015-default-device-follows-the-machine.md)). That is a choice made
before any provider is requested, not a replacement after a failure.

## Consequences

- The status line and the OBS log carry an observable reason; the user chooses
  CPU explicitly if they want it.
- ONNX Runtime may still assign individual unsupported operators to the CPU
  inside an accelerated session; provider profiles in the worker checks show
  the placement.
- Missing-model, provider, backend, and device failures are part of the test
  suite (`check_failure_modes.py` on Windows).

## Evidence

- [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md), *Inference*.
- `README.md`, *Processing Device*.
- `src/backends/backend.cpp`, `normalize_backend` and `configure_provider`.
