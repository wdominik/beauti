# Four-stage OBS finishing

The pipeline is Retouch → Skin Tone → Micro Dodge & Burn → Smooth. Defaults
are 60%, 50%, 50%, and 50%. Smooth uses Texture. Model inference runs in the
native worker; the graphics thread applies finishing to the matching snapshot.

Skin Tone uses D65 Lab chroma correction, a 256/384-pixel field, three separable
box passes, 64×64 median samples, and two histogram levels with 0.25-Lab-unit
bins. Its RGB8 endpoints blend in linear light. Dodge & Burn and Texture use
full-resolution face-region grids in both quality modes. Correction fields apply
to original-resolution pixels; they do not resize an RGB result into the frame.
Source alpha is preserved, with unpremultiplied internal color and premultiplied
output. Background pixels outside the region pass through. Hard semantic
exclusions and soft confidence weights remain active.

Mask erosion and feathering run through the shared OBS graphics path for every
inference provider ([decision 0005](decisions/0005-shared-gpu-mask-preparation.md)).
The pure CPU reference remains available.

### Micro Dodge & Burn

The GPU follows the independently authored C++ reference in
`src/effects/dodge_burn.cpp`:

- Pack mask-weighted L* and normalization into float textures. Three horizontal/
  vertical box pairs estimate the fine base, with replicated borders.
- Normalize the fine base, then pack base×mask, base²×mask, and mask. Three more
  box pairs estimate the broad target and variance together.
- Use the same face-scaled radii, variance protection, lightness gates, and
  bounded ±4 L* correction documented in DODGE-BURN.md. Apply the requested
  strength to the lightness change before gamut limiting and RGB8 rounding.
- Adjacent box samples are paired through linear interpolation. This implements
  the same uniform kernel with fewer texture instructions; it is not an
  additional approximation of the neighborhood size.

### Texture Smooth

The GPU follows `src/effects/texture.cpp`: four stationary separable five-tap
binomial levels, original-L* range weights in both passes, mask-normalized
neighborhoods, reflected borders, and face-scaled tap spacing. Thresholds are
6/7/9/12 L*, detail gains 0.45/0.85/0.70/0.20. No downsampling is used. Intermediate
textures retain the original lightness guide, mask, current level, and accumulated
correction. Alternating completed-level textures prevent read/write feedback.

The correction is bounded by `6*tanh(delta/6)` and multiplied by the skin mask.
Lab a*/b* remain fixed; twelve-step gamut backoff reduces L* change when needed.
The quantized endpoint blends with the actual preceding-stage result in encoded
sRGB at the Smooth strength. The finest band retains some texture; this is
contrast attenuation, not face generation or a semantic pore classifier.

## Continuity and numerical differences

All four controls have independent 150 ms smoothstep ramps. Retargeting begins
at the current value, not zero. Strength updates retain valid maps and can
compose between inference results. Zero disables only its stage; all-zero shows
the matching original snapshot during its ramp, then live bypass. The bounded
latest-frame queue and structural-setting invalidation remain in place.

The shared [output continuity implementation](OUTPUT-CONTINUITY.md) separates
freshness from bounded stall handling, bridges one bounded detection miss, and
retains complete compatible processed snapshots across temporary delays on CPU,
NVIDIA, and Apple. Native Apple integration validation remains pending.

RGBA32F intermediate textures limit accumulation error; final output is SDR
RGBA8. GPU float interpolation/rounding differs slightly from CPU double sliding
sums. Skin Tone's color statistics are sampled. A face box scaled from the
analysis frame can have fractional pixel coordinates: its field dimensions are
rounded, while corrections still apply to original pixels. Different detection
or model resolutions can also produce different masks. CPU references and GPU rendering therefore share algorithms and order without
promising a bit-identical end-to-end image.

No HDR processing or temporal prediction stabilization was added. The largest
face is processed. Cost scales with face area; these measurements do not certify
30 FPS for every possible near-full-frame face or every OBS scene.


## Validation and origin

[TESTING.md](TESTING.md) describes reference, GPU, worker, transition, and package
checks; they pass under Metal/OpenGL on macOS and Direct3D 11 on Windows.
Fixture timing is not a camera or encoding performance guarantee.

The GPU and CPU finishing implementations are independently authored project
code under GPL-3.0-or-later. No commercial product or paper-author source,
weights, shaders, SDK, binaries, or example images were imported for these stages.
[Skin Tone](SKIN-TONE.md), [Texture](TEXTURE.md), and
[Dodge & Burn](DODGE-BURN.md) record the sources, constants, and limitations.
