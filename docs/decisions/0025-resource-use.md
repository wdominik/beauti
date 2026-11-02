# 0025: Spend CPU, GPU, and memory only on what the output needs

Status: Accepted for development. Date: 2026-11-02.

## Context

A live filter shares the machine with the encoder and often a game. Time that Beauti holds
without using it is taken from them.

## Decision

- ONNX Runtime pools sleep between analyses instead of spinning. A short analysis pays a little
  wake-up time; the time between frames stays free.
- The retouch model stops two seconds after Retouch reaches zero. With all effects at zero,
  captures and analysis stop after two seconds, and the output is the source itself. Effects
  return after the next analysis, through the usual ramp.
- After three minutes without frames to analyze, the engine releases its models and loads them
  again on the next frame. On macOS that takes well under a second; the first load on an NVIDIA
  GPU still compiles.
- Intermediate GPU fields use half floats. The tone histograms, which count up to 4096 samples,
  and the Micro Dodge & Burn statistics, which hold squares, stay in 32-bit floats. Pixels change
  by at most a few 8-bit steps in rare cases, which the reference checks accept.

- The finishing fields are at most 1536 px on their longer side. A larger face region is computed
  at that size and sampled back, because its cost otherwise grows faster than its area. At 4K with a
  face close to the camera the OBS frame time falls from about 53 to about 20 ms, and the output stays
  practically the same (99% of the channel values within 2 of 255).

## Rejected after measuring

- Persistent threads for the mask: starting them takes well under 0.1 ms.
- A shared engine for several filters: two workers at once barely slowed each other down, and
  sharing would serialize their analyses.
- Skipping analysis of unchanged frames: it helps still images only, and would have to keep the
  continuity deadline alive.
