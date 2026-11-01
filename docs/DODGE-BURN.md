# Micro Dodge & Burn

Micro Dodge & Burn is the third independent 0–100% control, between Skin Tone
and Texture Smooth. Its default is 50%. To isolate it, set the other controls
to zero. All controls have independent 150 ms transitions.

[Retouch4me Dodge&Burn](https://retouch4.me/dodgeburn) is an appearance and workflow
reference. No proprietary code, models, SDK, binaries, or example images were
imported. Beauti does not reproduce its AI or claim equivalent quality.

`src/effects/dodge_burn.cpp` is independently authored C++20 under
GPL-3.0-or-later. It reuses project D65 color math and sliding-box averaging.
`data/beauti.effect` provides the corresponding full-resolution OBS GPU passes.

The method estimates a smooth local lightness correction, then adds it to the
original L* channel rather than replacing the image with a blurred base:

1. Convert RGB8 sRGB to D65 CIELAB. Keep a* and b* fixed.
2. Estimate a fine base using mask-normalized, three-pass box averaging. Radius
   is `clamp(round(min(width,height) * 0.0025), 1, 12)` pixels. Estimate a broader
   local target from that base with radius
   `clamp(round(min(width,height) * 0.018), 3, 80)`. The three passes approximate
   Gaussian smoothing; they do not use an imported filter implementation.
3. Estimate local base variance at the larger scale. An edge guard
   `1 / (1 + (variance / 36)^2)` reduces corrections near pronounced contours.
   Smooth lightness gates avoid near-black and near-white regions: fade in
   between L*=3 and 15 and fade out between L*=85 and 98.
4. Compute `4 * tanh(0.85 * (target - base) / 4)` L*, multiplied by the requested
   amount, skin mask, edge guard, and lightness gates. The correction is bounded
   to ±4 L* before these weights. It lightens local dark deviations and darkens
   local bright deviations while leaving fine detail in the original channel.
5. Keep a*/b* fixed. If a corrected pixel leaves sRGB gamut, reduce its L*
   correction with twelve bisection steps. Convert back to RGB8. Small chroma
   differences remain from output quantization. Zero strength and mask-zero
   pixels are exact bypasses.

These scales and thresholds are project choices. This is a statistical local
correction, not semantic identification of unwanted pore shadows or an expert
retoucher's decisions. Fine lines, facial shading, and imperfections can overlap
in size. The effect is deliberately bounded and can look subtle. Some overlap
with Texture remains; the isolated comparison is needed to judge its value.


CPU checks in `tests/reference/test_dodge_burn.py` cover correction direction,
fine-detail retention, chroma, hard edges, exclusions, translation, and strength.
See [GPU composition](FINISHING-OBS.md) and [test procedures](TESTING.md).
