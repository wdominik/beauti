# 0024: Read captures back at once when it is cheap; automatic analysis rate

Status: Accepted for development. Date: 2026-11-02. Changes the frame rate default of
[0008](0008-control-defaults.md).

## Context

The output shows the frame that was analyzed, so the delay of the analysis is the delay of the
source. The analysis takes less than one frame at 30 FPS, yet a capture waited a whole frame
for its readback and its result another one. A fixed 30 FPS target also left a 60 FPS canvas at
30 without saying so, and a target the analysis nearly reaches repeats frames at irregular
moments, which looks worse than an even lower rate.

## Decision

- A capture is read back in the tick that issued it when the analysis can finish before the next
  capture. The result then arrives one tick earlier.
- A wait above 30% of the frame interval returns to the later readback for 5 seconds, doubling up
  to 60 seconds. It never waits repeatedly on a saturated GPU.
- The default target frame rate is Automatic: the canvas rate divided by a whole number, up to
  60 FPS. The divisor grows only on evidence: results that stay below 90% of the capture rate in two
  windows in a row, or an OBS frame time above 85% of its budget, the latter not below 15 FPS. A
  long analysis alone lowers nothing while its results arrive. The divisor shrinks after 10 stable
  seconds when the 90th percentile of the analysis time is within 85% of the shorter interval. An increase
  that fails within 5 seconds doubles that wait, up to 120 seconds. A canvas above 40 FPS starts at
  half its rate.
- A fixed target stays available and is followed exactly.

## Consequences

- The delay at a 30 FPS canvas falls from about two frames to about one. The graphics thread
  waits for the GPU briefly at each analysis.
- The first immediate readback under a saturated GPU can cost a frame before the policy reacts.
- A machine whose analysis nearly fits at 60 FPS runs at an even 30 instead of an uneven 50.
- The OBS frame time includes everything else OBS renders, so a busy scene can also lower the rate.
