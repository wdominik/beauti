# 0006: Bounded last-good hold, one-miss bridging, 150 ms transitions

Status: Accepted. Date: 2026-10-25.

## Context

A fixed expiry (200 ms after capture on the GPU, a separate adaptive rule on
the CPU) was measured in the mask-feather investigation: it drops an already
displayed result before the next one arrives and produces a one-frame flash of
the unprocessed source during ordinary scheduling jitter. Continuity, freshness, and
bounded latency cannot all be guaranteed at once when processing is slower than
the source.

## Decision

One provider-independent policy in `src/core/continuity.h`:

- Keep the last complete, compatible result until a successor is accepted. The
  hold window is `clamp(2 * interval_ms + 200, 350, 2500)` ms from acceptance,
  where `interval_ms = max(last analysis time, 1000 / requested fps, observed
  delivery interval)`, additionally capped at five seconds from capture.
- Bridge one completed no-face result after a valid face for
  `clamp(1.25 * interval_ms + 75, 150, 1500)` ms; a second consecutive miss ends
  the hold.
- Transitions to and from the live source cross-fade over 150 ms; control
  changes ramp over 150 ms with exact zero endpoints. The filter never warps
  stale masks onto a newer frame.
- Recovery after a stall, error, or no-face timeout needs two consecutive valid
  results; startup needs one.

## Consequences

- Original flashes caused by scheduling jitter are regressions; repeated
  processed frames are reported separately and never counted as throughput.
- Maximum retained-image contribution is 5.15 s from capture; a hung worker
  cannot extend its own window.
- A fixed 150 ms first-miss grace period was prototyped and rejected: the CPU
  fixture needs roughly 800 ms per analysis, so no successor could arrive in time.
- The observed delivery interval is part of the formula because the `delay`
  fault mode shows a 250 ms flash under OBS 33 frame timing when only the engine
  time is used: the engine cannot see the scheduling delay between inference and
  delivery.

## Evidence

- [`docs/OUTPUT-CONTINUITY.md`](../OUTPUT-CONTINUITY.md), the contract.
- CTest `continuity`;
  `tests/obs/check_continuity.py` fault modes miss, misses, delay, stall, error, cut.
- `delay` fault-mode runs under OBS 33 (`tests/obs/check_continuity.py`).
