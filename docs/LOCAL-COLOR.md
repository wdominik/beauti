# Local Color Smoothing

This optional GPU stage follows Retouch and Skin Tone, before Micro Dodge & Burn,
Smooth, and Eyes. It reduces small skin chroma variations rather than changing
lightness or increasing the existing regional Skin Tone correction. It uses the
existing skin segmentation and introduces no models, downloads, or inference work.

Strength 0 skips the stage completely; the selected preset sets its initial strength. There is no enable property
or algorithm selector. All controls are stored as ordinary OBS settings and can
be included in presets.

## Processing

1. Evaluate Retouch and Skin Tone on the full-resolution face grid and convert to
   D65 Lab. Store a*, b*, L*, and skin weight in `LocalInit`.
2. Run two horizontal/vertical five-tap binomial rounds, with weights 1,4,6,4,1.
   Tap spacing is `clamp(face_short_side * .012 * size / 100, 1, 32)` pixels.
   Linear sampling supports fractional sizes. The original Lab guide and mask
   remain fixed throughout; only a*/b* are filtered.
3. Normalize each neighborhood using the skin mask and range weight
   `1 / (1 + (deltaL * edges / 8)^4 + (length(deltaAB) * edges / 6)^4)`.
   Here `edges` is the edge control divided by 100. Zero disables range weighting,
   but does not disable semantic exclusions or the face-border fade.
4. Subtract the original a*/b* from the filtered field. Bound the correction's
   length by `6 * tanh(length / 6)` and multiply by strength, skin weight, and
   the existing lightness protection. Preserve L*. Back off in twelve bisection
   steps if the corrected color would leave sRGB gamut; quantize the RGB endpoint.
5. Materialize Retouch, Skin Tone, and Local Color once in a full-frame half-float
   target (`LocalComposite`). The subsequent stages read it with the early
   strengths set to zero. This avoids applying them twice, another premultiplied
   RGBA8 intermediate, and a ninth sampler in the final Composite technique.

Source alpha and pixels outside the face are preserved. The local correction is
zero in excluded skin regions. RGB8 rounding can introduce a small lightness
change even though the algorithm holds L* constant. Large strengths or areas can
still remove desirable skin color variation; this is a configurable treatment,
not a claim of automatic makeup or lighting reconstruction.

## Reference and validation

The stage runs only in the OBS compositor. Its independent numerical reference
is `tests/obs/check_configurable_finishing.py` (NumPy), instead of adding another
unused production C ABI to `src/effects`. It tests a controlled chart with fine
luminance texture, spatial chroma variation, an abrupt color boundary, hard mask
exclusions, and alpha. The reference uses exact floating-point bilinear taps,
not OpenCV's quantized remap coordinates. `tests/native/finishing_checks.h`
produces the real GPU results and checks bypass/cache behavior and zero bands.

The existing fixed-profile C++ Texture reference remains unchanged. The same
NumPy oracle separately checks four individually selected texture bands plus a
different size/edge setting. Default-output regression still uses all 81 legacy
Retouch/Tone/Dodge/Smooth combinations and the archived previous bundle.
