# Source and model provenance

Recorded October 24, 2026. This register distinguishes copied
source and weights from independently implemented algorithms and conceptual or
appearance references. It records identity and attribution.

## Records

| Record | Purpose |
| --- | --- |
| `models/sources.json` | Original asset URLs, byte counts, and hashes |
| `models/manifest.json` | Prepared runtime model hashes (ONNX graphs and Core AI assets) |
| `docs/provenance.json` | Component origins, changes, notices, and source hashes |
| `buildspec.json` | OBS, SIMDe, ONNX Runtime, and TensorRT RTX dependency versions, archives, hashes |
| `COPYING`, `licenses/` | License texts and retained notices |

`uv run beauti-verify-provenance` checks the source snapshot in a fresh checkout.
Add `--models` to check prepared model files. `uv run python -m beauti_tools.models.download --check`
checks acquired originals and copied export source. Hashes identify the acquired
bytes.

## Components used

| Component | Origin and use | License evidence and changes |
| --- | --- | --- |
| Retouch U-Net | [ModelScope model](https://www.modelscope.cn/models/iic/cv_unet_skin_retouching_torch/) and [architecture](https://github.com/modelscope/modelscope/tree/master/modelscope/models/cv/skin_retouching) | Apache 2.0; notice in `licenses/ModelScope-NOTICE.txt`. Two copied architecture files remain hash-identical. Beauti exports weights and creates graph/precision variants, including Core AI assets whose decoder upsampling is rewritten as an equivalent matrix product. |
| Skin segmentation | [MediaPipe Multiclass Selfie Segmentation](https://storage.googleapis.com/mediapipe-assets/Model%20Card%20Multiclass%20Segmentation.pdf), 256 px | Apache-2.0; notice in `licenses/MediaPipe-NOTICE.txt`. Converted to ONNX and Core AI; outputs verified against the TFLite model. Beauti uses the face-skin class and removes FaceMesh feature regions. |
| Face and eye landmarks | Google MediaPipe FaceMesh V2 (yakhyo ONNX conversion) and Iris (PINTO ONNX conversion) | Apache-2.0; notices retained; byte-identical ONNX weights, independent C++ crop/mask code. See [Eyes](EYES.md). |
| Face detection | [MediaPipe BlazeFace full range](https://storage.googleapis.com/mediapipe-assets/MediaPipe%20BlazeFace%20Model%20Card%20(Full%20Range).pdf) | Apache-2.0; notice in `licenses/MediaPipe-NOTICE.txt`. Converted to ONNX; weights and operations unchanged. |
| ONNX Runtime | [Microsoft](https://github.com/microsoft/onnxruntime) runtime and C API headers | MIT and third-party notices retained. API level 31; runtime 1.31.0 on macOS and Windows (`buildspec.json`). Mac library install name is adjusted for relative loading. |
| OBS / SIMDe | [OBS 33.0.0](https://github.com/obsproject/obs-studio/tree/33.0.0), [SIMDe v0.8.2](https://github.com/simd-everywhere/simde/tree/v0.8.2) | OBS GPL-2.0-or-later, SIMDe MIT. Build headers and host linking; no host-source modifications. Notices are copied into bundles. The plugin uses GPL-3.0-or-later with OBS. |
| Skin Tone | [Portrait Engine algorithm notes](https://github.com/chacalra/portrait-engine/blob/main/docs/ALGORITHMS.md), OpenCV/W3C color math | Independently authored chroma correction; no reference source or new weights imported. See [Skin Tone](SKIN-TONE.md). |
| Micro Dodge & Burn | [Retouch4me](https://retouch4.me/dodgeburn) appearance/workflow reference | Independently authored bounded local L* correction, no proprietary code or weights. See [Dodge & Burn](DODGE-BURN.md). |
| Texture Smooth | Adobe Texture, darktable contrast equalizer, Local Laplacian research | Concept/appearance references only; independent scale-selective L* attenuation. No reference source or weights imported. See [Texture](TEXTURE.md). |

Native project code is GPL-3.0-or-later with an
[additional permission](../licenses/GPL-Additional-Permission.txt) for NVIDIA's
runtime, which users install themselves and packages do not contain. Third-party
notices and the corresponding source accompany every package. Product and paper
citations are references, and no vendor quality-equivalence or affiliation is claimed.
The complete reference links and departures are in the algorithm documents and
machine-readable register.

## Derived model lineage

1. `beauti_tools.models.export_model`: PyTorch to ONNX opset 17.
2. `beauti_tools.models.prepare_models`: fixed shapes, output selection, basic ONNX
   optimizations, and zero-padding no-op removal.
3. `beauti_tools.models.prepare_fp16`: precision conversion with float32 interfaces
   and duplicate-cast removal.
4. `beauti_tools.models.prepare_native_models`: the 384 px retouch variants.
5. `beauti_tools.models.prepare_eye_models`: FaceMesh V2 and Iris, unchanged.
6. `beauti_tools.models.prepare_mediapipe`: BlazeFace and Multiclass Segmentation from
   TFLite to ONNX (tf2onnx), planar input, renamed tensors, and an equivalent Resize
   coordinate mode; outputs verified against the TFLite models.
7. `beauti_tools.models.prepare_coreai`: Core AI assets for macOS.

Skin Tone, Dodge & Burn, and Texture introduce no additional learned weights.
Build and test Python packages are development tools; the OBS bundle contains
native runtime libraries only. The prepared model and packaged runtime hashes are recorded separately
from source hashes.
