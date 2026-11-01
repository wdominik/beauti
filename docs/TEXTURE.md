# Texture Smooth

Smooth follows Retouch, Skin Tone, optional Local Color Smoothing, and Micro Dodge & Burn.
Its 0–100% control blends with the actual preceding-stage result. The CPU reference
is `src/effects/texture.cpp`; OBS runs the corresponding GPU effect passes.

## Origins

[Adobe Texture](https://blog.adobe.com/en/publish/2019/05/14/from-the-acr-team-introducing-the-texture-control)
is an appearance reference, and [darktable's contrast equalizer](https://docs.darktable.org/usermanual/development/en/module-reference/processing-modules/contrast-equalizer/)
is a concept reference for scale-selective detail control.
[Local Laplacian Filters](https://people.csail.mit.edu/sparis/publi/2011/siggraph/)
and [Fast Local Laplacian Filters](https://imagine.enpc.fr/~aubrym/projects/llf/index.html)
provide background for edge-aware multiscale processing. No source, weights,
shaders, or binaries from these references were imported. This independently
written stationary decomposition does not reproduce those products or algorithms.
Color conversion uses the project's documented [D65 math](SKIN-TONE.md).

### Algorithm choices

1. Convert the face crop from RGB8 sRGB to D65 CIELAB. Keep a* and b* fixed and
   operate on L*. No texture synthesis, face generation, or geometry changes.
2. Decompose L* at four full-resolution scales without downsampling. Each level
   uses separable five-tap binomial weights `[1, 4, 6, 4, 1]`, reflected borders,
   mask-normalized neighbors, and range weight `1 / (1 + (deltaL / threshold)^4)`.
   Both passes use the original L* as their range guide. Protected neighbors
   with mask zero do not enter the filter's statistics.
3. Base spacing is `clamp(round(min(width, height) / 700), 1, 6)` pixels; four
   levels multiply it by 1, 2, 4, and 8. Range thresholds are 6, 7, 9, and 12 L*.
4. Attenuate the four detail bands by 0.45, 0.85, 0.70, and 0.20 at full strength.
   This retains some finest texture and the coarsest base. All orientations are
   processed.
5. Bound the summed correction with `6 * tanh(delta / 6)` L*, then multiply by
   skin confidence. Keep a*/b* fixed. If the result is outside sRGB gamut, use
   twelve bisection steps to reduce the L* change instead of desaturating.
6. Convert back to RGB8. The Smooth control blends this endpoint with the preceding pipeline result in
   encoded sRGB. RGB8 rounding introduces
   small chroma errors. Zero strength and excluded pixels have exact bypasses.

These constants are project choices, not values from Adobe or a
trained pore detector. Detail size alone cannot distinguish every pore from
fine hair, wrinkles, or noise. A broad correction can still look overly smooth
at high strength; user preference remains the acceptance criterion.


The tests in `tests/reference/test_texture.py` cover exclusions, range protection,
detail attenuation, chroma, translation, and strength progression. See
[GPU composition](FINISHING-OBS.md) and [test procedures](TESTING.md).

## Adjustable detail profile

In Manual mode, the Smooth section exposes four detail reductions (0–100%, reference profile
45/85/70/20), Structure Size (50–200%, reference value 100), and Edge Protection
(0–200%, reference value 100). The existing Smooth strength remains an independent 0–100%
blend. The reference values (the Low preset) reproduce the fixed profile exactly; the selected preset supplies the starting values in Manual.
Larger reduction values remove more contrast from that band. A smaller finest
band value can retain microtexture while higher middle-band values even out relief.

Structure Size scales the four tap offsets. The stationary texture filter still
uses reflected point samples, so neighborhood changes resolve to pixel steps.
Edge Protection multiplies `deltaL / threshold` before the fourth power. Higher
values reject more edge-crossing neighbors; zero retains mask protection but
disables lightness range weighting. The correction cap and color/gamut handling
remain unchanged. All settings use 150 ms render-thread ramps and reuse analysis.
