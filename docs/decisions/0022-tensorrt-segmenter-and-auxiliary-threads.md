# 0022: The segmenter runs on TensorRT RTX; auxiliary CPU threads follow the cores

Status: Accepted for development. Date: 2026-10-28. Refines the fixed auxiliary budget
in [0016](0016-cpu-inference-thread-sizing.md) on Windows.

## Context

With the segmenter on the CPU next to TensorRT RTX, the retouch model waited for the
mask and limited 4K delivery. More segmenter threads did not solve it.

## Decision

- With TensorRT RTX selected, the skin segmenter runs on the same device as the
  retouch model, with its own provider cache below the engine cache. CPU and Core AI
  placement are unchanged.
- The detector, FaceMesh, and Iris stay on the CPU (0002 still applies), because each
  extra TensorRT session delays the first processed frame.
- CPU sessions next to an accelerator use a quarter of the physical cores, at least
  two and at most four (`auxiliary_threads_for`). Windows measures the cores; other
  platforms keep two until measured. FaceMesh and Iris use this budget in every
  mode, because they overlap the retouch and mask models.

## Consequences

- More analyses per second mean more GPU work beside OBS, which matters most at a
  60 FPS canvas; macOS shows the same effect. Deferring the blocking readback map by
  one tick did not help and was rejected.
- The thread rule is unmeasured above four cores and under OBS encoding load.
