# Apple acceleration

On macOS, Beauti runs the retouch U-Net on the Neural Engine and the MediaPipe skin
segmenter on the GPU, in parallel, through Core AI. Core AI ships with macOS 27, which
is the minimum version ([decision 0018](decisions/0018-core-ai-replaces-core-ml.md));
the models are described in [decision 0021](decisions/0021-mediapipe-detection-and-segmentation.md).

## Configuration

| Model | Performance | High Quality | Runs on |
| --- | --- | --- | --- |
| BlazeFace face detector | 192 px | 192 px | CPU, ONNX Runtime |
| Retouch U-Net | `coreai/retouch-384.aimodel` | `coreai/retouch-512.aimodel` | Neural Engine, float16 |
| Skin segmenter (MediaPipe Multiclass) | `coreai/skin-segmenter.aimodel`, 256 px | same | GPU, float32 |
| FaceMesh V2 | 256 px | 256 px | CPU, ONNX Runtime, two threads |
| Iris (only for Eyes) | 64 px | 64 px | CPU, ONNX Runtime, two threads |

The device setting offers **Apple Acceleration** (Core AI) and **CPU** (ONNX
Runtime for every model). ONNX Runtime ships in the bundle for the CPU device and
the CPU models; its Core ML provider is not used.

While the retouch model runs, a second thread runs the segmenter and, overlapping
it, FaceMesh followed by Iris when Eyes is active; both finish before the retouch
result, so the mask and the eye correction add no time to the analysis.

### Adapter

Core AI has only a Swift interface. `src/backends/coreai.swift` is built into
`libbeauti_coreai.dylib` and exposes three C functions (`src/backends/coreai.h`):
load an asset with a preferred compute unit, run one input, release. The C++ side
(`CoreAIModel`, `src/backends/coreai_model.h`) owns the handle and turns failures
into exceptions. Each call blocks the engine's worker thread until Core AI has
finished; the graphics thread never calls it. Input tensors are allocated once per
model and refilled.

The compute unit is set explicitly. Core AI's automatic choice fails to load the
retouch model on the Neural Engine (error 53, "no memory") although memory is free.

Core AI caches compiled models per process under
`~/Library/Caches/coreai-cache/<macOS build>/<bundle identifier or executable>` (OBS:
`com.obsproject.obs-studio`). When the code signature of that process changes, as
with a rebuilt or updated host, Core AI fails every later load with `nilError`
instead of recompiling. After that error the adapter clears the process's cache
folder and loads once more; Core AI rebuilds the cache. Other load failures keep
the cache. This was
reproduced by re-signing the bench worker; the release gate in
[RELEASE.md](RELEASE.md) covers an OBS update.

### Model assets

`uv run beauti-prepare-models` runs `beauti_tools.models.prepare_coreai`, which
converts the models without retraining:

- Retouch, from the original PyTorch checkpoint with Apple's converter, Conv/BatchNorm
  fused and float16. The decoder's bilinear x2 upsampling with `align_corners=True`
  is not a resize the Neural Engine supports; it is written as two multiplications
  with fixed interpolation matrices, which is the same computation (maximum
  difference `3e-7`).
- Skin segmenter, from the ONNX graph of `prepare_mediapipe` with `coreai-onnx`. Its
  three TensorFlow-style nearest-neighbor resizes are already expressed with the
  equivalent standard coordinate mode there. On the GPU its class labels match the
  ONNX CPU result exactly.
- Debug locations are stripped before saving, so no build paths reach the bundle.

The Core AI bytecode writer does not order operations deterministically, so each
conversion yields new file hashes. The preparation step writes them to
`models/manifest.json`; the build and the bundle audit check the assets against it.

## Performance

M4 Pro, macOS 27.0.1, fixture measurements without a camera:

| | High Quality | Performance |
| --- | ---: | ---: |
| Native analysis (detect, prepare, all models, maps) | 21.4 ms | 13.4 ms |
| Face detection (CPU) | 2.1 ms | 2.1 ms |
| Retouch model call | 17.9 ms | 10.4 ms |
| Mask thread: segmenter 8.5–9 ms with FaceMesh 4 ms overlapping | 9.3 ms | 8.8 ms |
| 4K in OBS: distinct processed FPS at a 30 FPS target | 30 | 30 |
| 4K in OBS: fixture frame age, median | 100 ms | 100 ms |
| 4K in OBS: average frame render time | 23.5 ms | 22.3 ms |

The segmenter shares the GPU with OBS; its share shows in the render time. Running
it on the Neural Engine instead (as float16) queued it behind the retouch model
(analysis 25.0 / 16.8 ms) and lowered the render time by only 2–3 ms; on the CPU the
analysis took 28–29 ms. 60 FPS canvases at 4K have not been measured.

## Output

The retouch maps are closer to the ONNX CPU reference than under Core ML (maximum
0.0013 against 0.0031). The segmenter's labels are identical to its ONNX CPU result.
A live-camera review on 2026-10-27 accepted the Core AI retouch and the MediaPipe
mask in both quality modes.

## Rechecks

After a macOS or Core AI tooling update:

1. Run `beauti-prepare-models`; it fails if a rewrite or conversion changes a model
   beyond its tolerance.
2. Confirm placement: load each asset with debug information enabled
   (`SpecializationOptions.with_debug`) and check the compute unit per operation.
3. Run the native and OBS checks in [TESTING.md](TESTING.md), and compare against
   the previous bundle with `tests/native/compare_workers.py`.

Open options: distinct FPS is capped by the 30 FPS target, so a 60 FPS target or
render slack would rank future variants; Core AI's ahead-of-time compilation could
shorten the first load.
