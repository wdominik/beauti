# 0019: Adjustable finishing parameters without mode switches

Status: Accepted for development. Date: 2026-10-27.

## Context

Smooth used fixed band reductions, structure size, and edge protection, and
there was no way to even out small skin color variations without also changing
lightness. More even skin had to be possible while the existing output stays
reproducible.

## Decision

- Keep the existing strength keys and ranges. Expose the four Smooth band
  reductions, structure size, and edge protection as settings; the former
  constants remain available as the reference profile.
- Add Local Color Smoothing between Skin Tone and Micro Dodge & Burn. Strength
  zero is its only off state and skips all of its passes.
- Group each effect's controls in a normal OBS group; no checkable groups and no
  algorithm selectors.
- All new parameters are ordinary saved settings, ramped on the render thread
  without restarting inference.
- Local Color and the adjustable Smooth parameters are verified against an
  independent NumPy reference (`tests/obs/check_configurable_finishing.py`)
  instead of a new C++ reference in `src/effects`.

Which values a filter uses is decided by the look presets (0020).

## Consequences

With the reference profile the output matches the previous bundle exactly
across all 81 Retouch/Skin Tone/Dodge/Smooth combinations. Local Color adds six
draw passes and five float render targets; at 1280x720 there was no render lag
after warmup, which is not a 4K or recording claim. The shared GPU path still
needs Direct3D validation on Windows and a live-camera review.

## Evidence

[Local Color](../LOCAL-COLOR.md) and [Texture](../TEXTURE.md).
