# Skin Tone

Skin Tone is the second independent 0–100% control. It equalizes regional skin
chroma while retaining lightness detail. The CPU reference is
`src/effects/skin_tone.cpp`; OBS uses the corresponding GPU passes. The existing
models supply retouching and the semantic mask; this stage adds no model.

## Algorithm

Input is an RGB8 sRGB face region after the requested Retouch blend and its
floating-point skin mask.

1. Decode the sRGB transfer function and convert linear RGB through XYZ D65 to
   floating-point CIELAB D65. The original L* channel is retained. This is not the
   D50-adapted Lab coordinate system used by CSS `lab()`.
2. Estimate separate median a* and b* values from pixels with skin confidence
   above 0.65 and L* between 15 and 85. At least 16 qualifying pixels are required;
   otherwise, return the input. There is no universal target skin color.
3. Compute a regional chroma field using normalized, masked low-pass estimates:
   filter weighted a* and b*, then divide by the identically filtered weight.
   Our implementation uses three separable box passes with replicated borders.
   Radius is round(0.025 × the shorter crop side), limited to 2–64 pixels.
   This smooths the correction field, not the displayed RGB image or its L*.
4. Move the regional field toward the face's median chroma. Retain the original
   pixel's finer chroma residual. Bound the correction vector to 10 Lab units,
   then multiply by 0.9, skin weight, and strength. Weight fades smoothly over
   L*=3–15 and 85–98 to protect deep shadows and highlights. These constants are
   Beauti's project choices, not parameters claimed from another tool.
5. Reconstruct with the original L*. If the candidate is outside sRGB, reduce its
   chroma correction with 12 binary-search steps. Do not clip substantial color
   excursions by changing luminance. Round to RGB8 only at the output.


Zero strength and mask-zero pixels have exact bypasses. The full RGB8 endpoint
is blended with the preceding stage in linear sRGB at intermediate strengths.
RGB8 rounding introduces small lightness differences. The GPU estimates the
regional field at 256/384 pixels, samples 64×64 locations for median statistics,
and uses two-level histograms with 0.25-Lab-unit bins. Corrections still apply to
original-resolution pixels. See [GPU composition](FINISHING-OBS.md).

## Sources

- The conceptual reference for regional chroma equalization was Portrait Engine's
  [algorithm documentation, section 5c](https://github.com/chacalra/portrait-engine/blob/main/docs/ALGORITHMS.md).
  It describes a masked low-frequency chroma correction toward median skin color
  while retaining L*. No repository source code, dependencies, or models were
  copied. Our filter, strength limits, luminance gates, gamut handling, and C ABI
  are independently implemented.
- [OpenCV's color conversion documentation](https://docs.opencv.org/4.13.0/de/d25/imgproc_color_conversions.html)
  and [W3C CSS Color 4, October 22, 2026 draft](https://www.w3.org/TR/2026/CRD-css-color-4-20261022/)
  were mathematical references for Lab/sRGB processing. No sample code was copied.
  OpenCV is used as a separate numerical reference in the tests.
- [Retouch4me Skin Tone](https://retouch4.me/skintone) was a visual/product reference
  for color equalization. Its software and models are not included, and this is
  not a reproduction of its proprietary algorithm or a claim of matching quality.


These references were recorded October 24, 2026. See [provenance](PROVENANCE.md).
Tests in `tests/reference/test_skin_tone.py` cover chroma reduction, lightness
retention, saturated colors, uncertain masks, small dimensions, and exact bypasses.
Mixed lighting or intentional makeup colors can be reduced along with unwanted
blotchiness. No temporal stabilization or HDR processing is provided.
