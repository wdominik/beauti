# 0001: Inference runs on one worker thread; the graphics thread only composes

Status: Accepted. Date: 2026-10-26.

## Context

OBS calls the filter's render callback on its graphics thread once per output
frame. The neural networks take from about 10 ms (Core ML, Performance, M4 Pro)
to roughly 800 ms (CPU fixture) per analysis. Running them inside the render
callback would stall every source in the scene.

## Decision

The plugin has exactly two threads of its own concern: the OBS graphics thread,
which captures the source, hands reduced frames to a worker, and composes the
result, and one worker thread per filter instance, which owns the `Engine` and
runs ONNX Runtime. The graphics thread never runs inference; the worker never
touches OBS graphics objects. Jobs and results pass through bounded queues
guarded by one mutex, and a generation counter drops work that belongs to an
earlier source, size, or settings context.

The worker never calls OBS module functions either. Strings it needs from the locale catalog are copied when the filter is created.

## Consequences

- Presentation always runs at the OBS frame rate; between analyses the last
  good result is held (see [0006](0006-bounded-last-good-hold-and-150ms-transitions.md)).
- Every structure the two threads share is listed in `CONTRIBUTING.md` under
  *Threads*; new shared state needs a reviewed ownership rule.
- The module-function rule follows a ThreadSanitizer finding: libobs frees a module's
  locale table during deferred source destruction while a worker may still be
  running, so `obs_module_text` from the worker is a data race at shutdown.

## Evidence

- [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md), data-flow diagram.
- ThreadSanitizer run of the fault-injection suite, 2026-10-26 (`CHANGELOG.md`,
  *Fixed*); zero reports with the rule in place.
