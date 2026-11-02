# Eyes

The independent Eyes slider enhances existing iris color and subtly lifts eye
whites. Its strength follows the selected look preset and fades over 150 ms.
In Manual it has its own slider. Try 25–40 for a restrained result; 100 is
still bounded. This is localized exposure
correction, not eye recoloring, pupil replacement, or synthesized detail.

## Detection and masks

The worker reuses the BlazeFace box and eye keypoints for a rotated face crop. The
same FaceMesh result also cuts eyes, brows, and lips out of the skin mask, so FaceMesh
runs whenever the skin stages are active, not only for Eyes.
FaceMesh V2 takes RGB float32 `[1,3,256,256]` in [0,1]; its 478 points provide the
16-point contours of each lid. A face-presence sigmoid below 0.8 disables eyes.
Each open eye gets a 2.3x-width, rotated 64px crop for the Iris model. The second
eye is mirrored on input and mapped back using MediaPipe's continuous landmark
coordinates. Only its five iris points are used: V2 lids remain authoritative.

Both models run on the CPU on every backend, with the auxiliary thread budget (two
threads, on Windows a quarter of the physical cores up to four). FaceMesh is
loaded with the engine because the skin mask needs it; Iris is loaded on first use
and kept. While the skin stages are active, Iris runs on the landmark thread right
after FaceMesh, overlapping the skin segmenter, so Eyes adds no analysis time (M4
Pro: 13.3 / 21.2 ms with Eyes, the same as without). No eye model runs once the
strength has faded to zero. If a model is missing or fails, the engine disables only Eyes until the
filter's engine is recreated and logs the reason once; the other stages continue.

## Model choice

Three landmark combinations were compared on an M4 Pro (ONNX Runtime CPU, two
threads) on four still images in seven variants each (exposure −1/−2 EV, noise,
blur, downscaling, 15° rotation), 120 jittered frames, and 300 frames of a
public video with several blinks:

| Combination | Median | p95 | Iris-center jitter, median of four images |
| --- | ---: | ---: | ---: |
| FaceMesh V2 | 2.36 ms | 2.73 ms | 0.15–0.55 px |
| FaceMesh V1 + Iris | 3.79 ms | 4.22 ms | 0.08–0.37 px |
| FaceMesh V2 + Iris (selected) | 5.03 ms | 5.51 ms | 0.11–0.23 px |

The Iris model roughly halves the iris-center jitter on camera images, which
matters for a local color effect. Its own lid contours close less tightly on
real blinks than V2's (lid area / width² 0.099 versus 0.023 in the same frame),
so V2's lids are kept. Both models stay on the CPU, because the Iris graph splits into many small
accelerator sections, which made the path slower than on the CPU.
Jitter is measured against the known synthetic displacement, not against
annotated ground truth.

The mask is a 512x512 RGBA float texture in the existing face ROI, with iris in R
and sclera in G. Polygon distance feathers inward by roughly 6% of eye width.
Eye width must be at least 12 analysis pixels. The lid-area/width² ratio fades
from zero at 0.075 to full confidence at 0.16; closed lids skip Iris inference.
Iris axes must be at least 2px, no more than 35% of eye width, and within a 2:1
aspect ratio. The center must lie inside the lid and agree with rim midpoints.
Nonfinite predictions are rejected. The inner 30% of the iris radius stays dark;
the ring fades in to 55% and out from 80–100%. Sclera starts outside 108–130% of
the iris radius, avoiding a bright halo on the iris boundary.

Masks are tied to the captured source frame and never smoothed or reused across
different frames. The existing presentation continuity rules still apply to the
whole captured image. There is no calibrated segmentation of pupil or glasses:
dark-pixel and highlight guards supplement geometry. Occlusion, glare, extreme
gaze, makeup, and very dark/noisy footage can still defeat landmarks; the effect
is deliberately conservative, and cannot recover detail absent from the source.

## Color correction

An optional final GPU pass operates on full-resolution straight-alpha RGB and
re-premultiplies the result. It is separate to stay under OBS's eight-texture
limit; zero skips the pass. Alpha and pixels outside the mask remain unchanged.
`src/core/tuning.h` sets maximum iris exposure to +0.45 stops, sclera exposure to
+0.16 stops, and iris chroma enhancement to 4%. Exposure preserves channel ratios;
chroma moves RGB slightly away from its luminance without replacing the hue.

Linear luminance gates shadow protection from 0.008–0.045 and highlight protection
from 0.55–0.9. Sclera additionally requires luminance 0.06–0.2 and fades away for
sRGB channel spread 0.12–0.32. The entire positive correction is reduced together
when needed to fit the RGB gamut. This avoids channel clipping and tinted whites.
The full formula is independently reproduced in `tests/obs/check_eye_pixels.py`.

## Model provenance

- FaceMesh V2: [yakhyo ONNX conversion](https://github.com/yakhyo/mediapipe-face-mesh-onnx),
  `face_landmarker_Nx3x256x256.onnx`, 4,864,717 bytes.
- Iris: [PINTO model 049](https://github.com/PINTO0309/PINTO_model_zoo/tree/main/049_iris_landmark),
  `saved_model_64x64/model_float32.onnx`, 2,683,089 bytes.

The [Google FaceMesh V2 model card](https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Face%20Mesh%20V2.pdf)
and [Iris model card](https://mediapipe.page.link/iris-mc) identify Apache-2.0 model
licenses. Conversion-repository licenses and attribution ship in `licenses/`.
The ONNX files are unchanged; no upstream implementation code is copied. Source
archives and output hashes are pinned separately in the model manifests.

## Validation

`beauti-tests [eyes]` checks closed/invalid eyes, protected pupil and outside-lid
pixels, RGB/alpha sampling, padding, rotation, and mirrored crops. The optional
last `eyes` argument to `beauti-gpu-check` requests real inference and emits
`eyes.f32` plus 25/50/100% outputs. `check_eye_pixels.py` checks those against the
independent reference, including exact alpha/outside-mask preservation. The GPU
harness also checks disabled eyes, cached redraws, missing maps and recovery.

The OBS fixture runner accepts a final `eyes` strength. Transition modes 8 and 9
exercise positive eye-slider dragging and zero/nonzero transitions respectively.
Platform-specific validation and limits are recorded with the implementation
evidence; Windows runtime/Direct3D acceptance must be performed on Windows.
