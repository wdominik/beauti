# Testing Beauti

`compare_workers.py --reference-backend` names the reference worker's backend when
it differs from the candidate's. Narrow map checks fail on real provider
differences; assess the separately recorded final pixels rather than loosening
them. CPU old/new comparisons require `--exact`.

The opt-in native `[provider-lifetime]` test covers concurrent engines, CPU/GPU
coexistence, failed construction, destruction, and recreation. Set
`BEAUTI_TEST_MODELS`, `BEAUTI_TEST_FIXTURE` (1280x720 RGBA), and
`BEAUTI_TEST_CACHE`, then run `beauti-tests "[provider-lifetime]"`. Put the matching
four NVIDIA DLLs beside the Windows test executable. Without input variables it
skips; GPU-less CTest does not invoke it.

Record cold/warm startup separately. `check_continuity.py` prewarms the continuity-test
bundle once without `BEAUTI_CONTINUITY_TEST` before the unchanged 3-11 second
fault-observation window. No last-good frame exists before first inference, and a
fresh bundle path (such as the pytest copy) starts without a TensorRT RTX cache. The test-only module
waits briefly for OBS to consume an injected miss, so fast engines cannot overwrite
the fault before the renderer observes it. Production scheduling is unchanged.
Selection tests allow 20 seconds for initial compilation and still require either
processed output or an explicit failure; they record first processed output.
The Windows slider suite records a four-second control lead-in for CPU and GPU
step tests (`control_start_seconds` in the harness JSON). Endpoint transitions
then occur at seconds 6/8/10/12 with the same pixel and 150 ms ramp checks. This
separates a provider still starting from a broken strength transition. Drag tests
and the independent startup/performance measurements retain their own windows.

Exact comparisons guard implementation-preserving changes. They are not a product
requirement for identical CPU/GPU results. Intentional approximations use the
separate [performance and image-quality criteria](PERFORMANCE-ACCEPTANCE.md),
including perceptual and temporal checks while retaining structural invariants.

The [output continuity contract](OUTPUT-CONTINUITY.md) additionally specifies
required deterministic overload, stall, recovery, and source-change tests across
CPU/TensorRT RTX/Core AI. CTest and `tests/obs/check_continuity.py` cover the shared policy
and native Windows, Apple Core AI, and Apple CPU fault injection.

`beauti-continuity-test` is a separate test-only module, never an install target.
To run the injection suite, prepare an isolated development bundle with that DLL
renamed to `beauti.dll` in the plugin folder, the matching runtime/models, and both effect
files including `data/continuity.effect` and `data/cache-key.txt`. Never replace an installed plugin or a
release bundle. Pass that bundle to `tests/obs/check_continuity.py` with `--harness`,
`--obs-runtime`, `--fixture` (1280x720 RGBA), `--backend 2` or `4`, and a fresh
`--output` directory. On macOS, place the separate `beauti-continuity-test.so` module at
`Contents/MacOS/beauti` in an isolated copy of a development bundle, then ad-hoc
sign that copy. Backend ID 0 selects Core AI. The macOS runtime
path defaults to the installed OBS frameworks; `--obs-runtime` can override it.
The production module ignores `BEAUTI_CONTINUITY_TEST`.
`tests/obs/check_source_status.py` tests the production module's source diagnostics
through real libobs properties and the Refresh Status button. Pass `--bundle`,
`--harness`, `--fixture`, `--obs-runtime`, `--backend`, and a fresh `--output` directory.
It covers a detached filter, zero dimensions, oversized width/height, unsupported
source/render color spaces, OBS color conversion, and resumed processing. Its
opt-in fixture controls exist only in `beauti-obs-check`, not in the plugin.
The pytest entry point runs CPU and the platform's default accelerator. On Apple,
run this check natively with Core AI and CPU, plus the continuity and slider suites
under ThreadSanitizer before committing render/threading changes.

`tests/build/test_publication.py` injects copy, audit, compression, checksum, rename,
and rollback failures. It checks that existing bundles/archives survive failures,
one previous bundle is retained after success, and failed rollback copies remain
recoverable. Mac-layout fixtures cover portable bundle publication; native Mac
signature verification and `ditto` packaging still require an Apple host.

The OBS fixture report's sample columns are time, source-frame ID, current ID,
effect difference, and white-channel fraction (used by the scene-cut test).

Run commands from the repository root. Use a dedicated libobs process for graphics
checks. The runners do not require the OBS UI or change saved scene collections.

## Running the tests

Three runners cover the repository; the rest of this document describes what the
individual checks establish.

- **CTest** runs the native tests: `uv run cmake --workflow --preset macos`, or
  `ctest --test-dir build/macos` after a build. They are Catch2 test cases in
  `tests/native/`, compiled into one executable, `beauti-tests`; the CTest names
  `continuity`, `pixel-quantize`, `provider-policy`, `pacing`, `ramp`, `scene-cut`,
  `live-stats`, and `face-tracker` select them by tag, and
  `build/macos/beauti-tests "[continuity]"` reports a failure with its values.
- **pytest** runs every Python test: `uv run pytest`. The build-pipeline tests need
  nothing; the numerical reference tests need the reference library
  (`uv run beauti-build-reference`); the integration checks need a staged bundle, a
  1280x720 face fixture, and the runners from the native build, configured through
  the environment variables documented in `tests/conftest.py` (`BEAUTI_BUNDLE`,
  `BEAUTI_FIXTURE_720P`, `BEAUTI_BUILD_DIR`, `BEAUTI_REFERENCE_BUNDLE`, ...). A
  missing prerequisite skips the test and names the variable. Markers select by
  prerequisite: `uv run pytest -m "not needs_bundle and not needs_gpu"` is what CI
  runs, `uv run pytest -m needs_gpu` the libobs graphics checks. The Windows CPU
  and TensorRT CI jobs also run the Python suite without bundle/GPU prerequisites.
  `tests/build/test_cmake_pins.py` reconfigures real compiler-free CMake projects
  across pin changes, including external path overrides.
  `tests/build/test_bundle_integrity.py` rejects missing/changed files, empty
  model directories, stale report labels, and damage to the packager's copied input.
- **Sanitizers**: the presets `macos-asan` (AddressSanitizer with
  UndefinedBehaviorSanitizer) and `macos-tsan` (ThreadSanitizer) build every target
  instrumented into `build/macos-asan` or `build/macos-tsan` and run CTest. Point the
  Python checks at such a build with `BEAUTI_BUILD_DIR` and a bundle copy whose
  `beauti-bench` and plugin module come from it; the fault-injection suite under
  ThreadSanitizer is the check for the worker/graphics-thread contract. The Swift
  Core AI adapter is not instrumented, like the system libraries it calls. A
  sanitized build is never packaged.

The `check_*.py` scripts remain command-line tools (the Windows validation runs them
directly); the `test_*.py` modules next to them call the same functions.

## Fixtures

The fixtures are derived from the public-domain NASA astronaut image shipped by
scikit-image ([source PNG](https://raw.githubusercontent.com/scikit-image/scikit-image/v0.25.2/skimage/data/astronaut.png),
[attribution](https://scikit-image.org/docs/stable/api/skimage.data.html#skimage.data.astronaut)),
SHA-256 `88431cd9653ccd539741b555fb0a46b61558b301d4110412b5bc28b5e3ea6cb5`. They are
test input only and are neither committed nor shipped. Save the PNG as
`.cache/fixtures/astronaut.png` and create the raw RGBA files with the pinned test
dependencies:

```python
from pathlib import Path
import cv2
import numpy as np

p = Path('.cache/fixtures')
rgba = cv2.cvtColor(cv2.imread(str(p / 'astronaut.png')), cv2.COLOR_BGR2RGBA)
rgba.tofile(p / 'astronaut-512.rgba')
face = cv2.resize(rgba[20:300, 90:370], (720, 720), interpolation=cv2.INTER_LINEAR)
canvas = np.empty((720, 1280, 4), np.uint8)
canvas[:] = [65, 70, 75, 255]
canvas[:, 280:1000] = face
canvas.tofile(p / 'face-720p.rgba')
cv2.resize(canvas, (3840, 2160), interpolation=cv2.INTER_LINEAR).tofile(p / 'face-4k.rgba')
lower = np.zeros_like(canvas)
lower[..., 3] = 255
lower[120:] = canvas[:-120]
lower.tofile(p / 'face-720p-eyes-lower.rgba')
```

| File | Size | SHA-256 |
| --- | --- | --- |
| `astronaut-512.rgba` | 512x512 | `0df3c62c654dd5432e753a8d273e73ad3fb7d5826848b395afaead620b89bdd0` |
| `face-720p.rgba` | 1280x720 | `240789d59d2c32fbcb002857450ee9a14000c37e9f0d88bb53991755afee47df` |
| `face-4k.rgba` | 3840x2160 | `7c8dc1d45061ca6f73e38db9382ec527e5563fe04da164593608c7f3f810b936` |
| `face-720p-eyes-lower.rgba` | 1280x720 | `1677611125cc2ee68cbb0957a7cd52f57de84da4b79b10a88928c42ff5846390` |

## Portable references

```sh
uv sync --group dev   # includes the test group (NumPy, OpenCV)
uv run beauti-build-reference
uv run pytest tests/reference
```

Twenty synthetic reference tests cover Skin Tone, Micro Dodge & Burn, and Texture:
exact bypasses and exclusions, color/lightness properties, scale behavior, edge
protection, small images, invalid input, and strength progression. OpenCV supplies
an independent color-conversion reference. It is not a plugin dependency.

## Native and package checks

The main build runs the Catch2 tests through CTest (continuity policy, input
quantization, provider policy). After building:

```sh
uv run beauti-verify-native dist/beauti.plugin --backend cpu
uv run beauti-audit-mac-bundle dist/beauti.plugin
uv run beauti-verify-provenance --models
```

The worker check sends two synthetic protocol frames, a bypass request and a
face-free image, and verifies that neither returns a map. It is not a face-quality
or performance benchmark. `beauti-bench` speaks the binary protocol documented in
`tools/benchmarks/worker.cpp` and `tools/beauti_tools/bench/protocol.py`: it returns
the retouch blend map and the raw skin mask, never composited pixels; compositing
is the GPU's job and is covered by the OBS GPU runner below. The bundle audit checks
Mach-O linkage, relative runtime search paths, signatures, and external symlinks
on a byte-identical file snapshot outside synchronized folders. Finder metadata
is excluded; signatures are never regenerated by the audit.
Use `beauti-audit-windows-bundle` on Windows instead. Both platform audits also
check the staged payload inventory described in [BUILD.md](BUILD.md). A Windows
audit recalculates the included report's artifact match; a matching report can
still contain failed checks. Neither payload identity nor CTest establishes
face-inference, live-camera, or sustained GPU acceptance.

For native Windows or macOS face-inference checks, run `tests/native/check_worker.py` with
`--bundle`, `--fixture`, `--width`, `--height`, `--backend`, and `--output`. It checks
framing, bypass frames without payload, map presence per request, finite map values,
repeat determinism, and the reported backend.
The backend is `cpu`, `coreai`, `tensorrt-fp32`, or `tensorrt`; `--quality full` selects the
512px models. Add `--profile` to record and check actual ONNX Runtime provider execution;
with `coreai` only the detector runs through ONNX Runtime, and the Core AI placement
check is described in [Apple acceleration](APPLE-ACCELERATION.md#rechecks). This diagnostic mode sets
`BEAUTI_ORT_PROFILE_DIR` in the child process. Leave that variable unset during
performance measurements. The normal plugin does not enable profiling.

## GPU and continuity checks

Provide a tightly packed RGBA8 fixture whose dimensions match the arguments.
No photographs are required by the source distribution. After a native build:

```sh
build/macos/beauti-gpu-check dist/beauti.plugin INPUT.rgba build/pixels WIDTH HEIGHT
python tests/obs/check_finishing_pixels.py build/pixels
```

The GPU runner evaluates 81 combinations of four controls, masked/transparent
cases, and stage order. The checker compares GPU pixels to CPU references,
checks alpha and protected pixels, and records tolerances. Supply an image you
have permission to use when evaluating face appearance.
An optional final backend ID (`0` Core AI,
`2` CPU, `3` TensorRT RTX FP16, `4` TensorRT RTX FP32) selects the
inference backend for the GPU runner; its default remains CPU.

`beauti-obs-check` runs fixed or moving fixtures and slider changes through libobs
on macOS (Cocoa/OpenGL) and Windows (Direct3D 11); see [OBS fixture runner](#obs-fixture-runner). Analyze its frame records with
`tests/obs/check_slider_transitions.py`. Measure distinct processed output frames,
render lag, and frame age; a high host-render rate alone is not inference speed.

## Scope and limits

Mac results do not establish Windows compilation, D3D11, TensorRT RTX, or RTX performance.
Fixture benchmarks exclude camera capture and encoding. Live-camera visual
acceptance and performance must be checked on the intended hardware and scene.

On Windows, `tests/obs/run_windows.py` automates isolated fixture runs, frame
counter analysis, US English control checks, and slider transitions. It accepts
`--cpu-bundle`, `--tensorrt-bundle`, `--harness`, `--obs-runtime`, `--fixtures`, and
`--output`. The fixture directory must contain `face-720p.rgba` (1280x720) and
`face-4k.rgba` (3840x2160); see [Fixtures](#fixtures).
The child process PATH contains only OBS binaries and Windows System32.
GPU memory samples include other applications and are not process-only VRAM.

`tests/native/check_failure_modes.py` checks unavailable TensorRT, missing models,
a missing provider DLL, and a hidden GPU without changing the driver. Pass
`--cpu-bundle`, `--tensorrt-bundle` (a test copy with the NVIDIA DLLs), and `--output`.

Build orchestration regressions run with `uv run pytest tests/build`. They check CPU/TensorRT RTX output
isolation, missing-input rejection, and whether changed runtime/model/effect
files incorrectly inherit a historical verification pass.

For changes to native preprocessing, `tests/native/compare_workers.py` compares
baseline and candidate bundles on a 12-frame corpus covering motion, changing
sizes, alpha, zero strength, no face, and face recovery. Detection boxes and the
returned blend maps and skin masks are compared texel by texel. Use `--exact` to
reject any changed map value and run both quality modes for each processing backend.
Both workers must implement the current maps protocol; for an incompatible
reference, compare final frames through the matching OBS runners.
`tests/obs/bench_performance.py --reference-bundle BASELINE --bundle CANDIDATE`
interleaves old/new builds and reverses the run order on the second repeat.
It supports both platforms and all native backends. macOS
uses installed OBS frameworks by default; Windows still requires `--obs-runtime`.
`--warmup-seconds` defaults to 3; this does not automatically exclude a long cold
model compilation. Inspect startup and passthrough counts separately.
Keep compilation and other GPU work outside timed runs.

`tests/obs/check_processing_defaults.py` tests unset settings on CPU/TensorRT RTX builds,
the NVIDIA default in both quality modes, explicit FP16/CPU selections, and the
same TensorRT bundle with its GPU hidden through `CUDA_VISIBLE_DEVICES=-1`. The latter
must select CPU for unset settings, display the fallback explanation, and report
an error for an explicitly selected NVIDIA device. This preserves the installed
driver and does not replace a test on a machine with a missing or old driver. Pass
`--cpu-bundle`, `--tensorrt-bundle`, `--harness`, `--obs-runtime`, `--fixture` (720p),
and a new `--output` directory. It checks the loaded plugin's actual selection and
processed output, not just the backend helper.

Opt-in `BEAUTI_TIMING=1` emits host-wall pipeline spans. Summarize a fixture log
with `tests/obs/analyze_pipeline_timing.py --log LOG --output JSON`; optionally add
`--profiles DIRECTORY` for separately collected ORT host profiles. These do not
measure GPU event/kernel time. The throughput runner clears both diagnostic
environment variables.

Opt-in `BEAUTI_STATS=1` measures a live session without the per-stage output. Every
ten seconds and when it is removed, each filter writes one `[beauti-stats]` line with
cumulative values to the OBS log: ticks, processed and repeated output, frame age,
analysis and compose time, no-face results, bridged analyses, scene cuts, face
losses by reason, recovery time, OBS frame time, and lagged frames.
`tests/obs/live_stats.py LOG` returns the last line of each filter as JSON, and
`tests/obs/bench_performance.py` records it for its runs.

`tests/native/run_clip.py` plays a frame sequence through `beauti-bench` without a
camera or OBS, so that every machine measures the same input: a video file (`--clip`),
or a still image in a seeded synthetic scenario (`--image`, `--scenario`). It reports
misses and their runs, empty skin masks, face box steps, and the frame-to-frame change
of the mask and the retouch map. Maps are compared in frame coordinates
(`tests/native/clip_metrics.py`), so a moving box does not count as flicker. The input
luma step gives the change that the source itself contains. These are model outputs,
not composited pixels, and each frame is analyzed as if analysis always kept up. Keep
clips of real people outside the repository.

```sh
cd tests/native
uv run python run_clip.py --bundle ../../dist/beauti.plugin --backend coreai \
  --cache /tmp/beauti-clips --output /tmp/beauti-clips.json --clip CLIP.mp4
```

The native graphics checker accepts `bundle input.rgba output_directory width
height [backend] [quality]`; quality 0/1 selects the actual Performance/High
Quality model dimensions. Its separate `high-quality.rgba` output selects the
finishing quality and alone does not prove that the 512px models were tested.
For raw mask maps, `check_finishing_pixels.py` reconstructs erosion/feathering
with OpenCV before applying the existing independent finishing checks.

The GPU runner also compares seven cached/uncached map scenarios byte for byte:
strength changes, replacement masks, raw/prepared format changes, dimension
changes, source changes, and alpha. A zero map revision disables caching for
reference renders. A selected quality now applies to the entire 81-control
matrix as well as model dimensions. Historical reports made before this harness
change retain their original scope and have not been relabeled.

## OBS fixture runner

`beauti-obs-check` is built by CMake and accepts positional arguments:

```text
beauti-obs-check BUNDLE FIXTURE_RGBA REPORT_JSON SECONDS QUALITY BACKEND RETOUCH SMOOTH WIDTH HEIGHT MOTION SLIDER_MODE RESERVED LOCALE TITLE SKIN_TONE DODGE_BURN
```

Quality: 0 = Performance, 1 = High Quality. Backend: 0 = Core AI, 2 = CPU, 3 = NVIDIA FP16 retouching, 4 = NVIDIA Full Precision, or the
literal `default` to test the plugin's own default. Slider mode: 0 = fixed,
1 = zero/nonzero steps, 2 = continuous positive dragging, 3/4 isolate Skin Tone,
5 drags all four controls, 6/7 isolate Dodge & Burn. Use at least 10 seconds for
transition checks, RESERVED=0, LOCALE=en-US (or another catalog in `data/locale/`),
TITLE=Beauti. The fixture must match the supplied dimensions and be tightly packed
RGBA8. The runner writes `REPORT_JSON` (frame samples: time, source-frame ID,
current ID, effect difference, white fraction), `REPORT_JSON.ui.json` (effective
settings), and `REPORT_JSON.rgba` (the final frame).

On Windows, set `BEAUTI_OBS_RUNTIME` to the extracted official OBS root and prepend
its `bin/64bit` directory to the test process's PATH so it can find `obs.dll`; the
runners also register that directory with `AddDllDirectory`, because libobs's module
loader ignores PATH. `beauti-gpu-check` links ONNX Runtime directly; copy the
matching bundle runtime DLLs beside it before selecting TensorRT. Always use a separate
process and never modify the user's scene collection. The headless warning about
unavailable frontend menu callbacks is expected.

The analysis scripts `check_finishing_pixels.py`, `check_slider_transitions.py`,
and `check_continuity.py` require NumPy; the production plugin does not.

## Eyes

The optional final argument to `beauti-gpu-check` requests eye inference. For a
fixture with visible eyes, run the GPU harness with `... WIDTH HEIGHT 0 0 100`,
then `uv run python tests/obs/check_eye_pixels.py OUTPUT_DIRECTORY`. This checks
25/50/100% against an independent reference, protected regions, alpha, zero and
missing-map behavior, and cached redraws. The existing 81 finishing combinations
still run with Eyes=0. CTest's `eyes` tag tests crop geometry and mask safety.

`beauti-obs-check` accepts a final eyes strength after dodge_burn. Slider modes 8
and 9 exercise eye-only positive dragging and zero/nonzero steps; analyze their
reports with `tests/obs/check_eye_transitions.py DRAG STEPS OUTPUT`. This uses a
small eye-specific pixel threshold and allows one second for fresh eye analysis
under TSan while requiring multiple intermediate fade frames and exact zero.
`check_continuity.py --eyes 100` runs the existing six faults with eyes enabled.
The runner measures its per-frame difference only in the image center (y 230–450,
x 540–790 of 1280x720); the eyes of `face-720p.rgba` lie above it, so eye-only modes
need `face-720p-eyes-lower.rgba`, which has the eyes inside that window. `beauti-bench --eyes N` runs the eye models in the worker for
timing; eye maps are not part of its protocol.

### Configurable finishing

The native GPU runner also produces `chart-color-*` and `chart-texture-*` results,
real-face comparison variants, and alpha/cache/bypass checks. Run
`uv run python tests/obs/check_configurable_finishing.py OUTPUT` for the independent
NumPy reference. The standard GPU pytest entry point includes this check.

The headless runner accepts `BEAUTI_FIXTURE_SETTINGS=/absolute/settings.json` to
merge additional settings before source creation. It exports effective settings
as `REPORT.settings.json`. Its flattened UI report retains each control's group,
and rejects checkable groups. Slider modes 10/11 exercise local-color dragging
(with size changes) and zero/nonzero transitions. Mode 12 drags Smooth detail
shape controls.

### Preset selection and persistence

Run `beauti-obs-check` with `BEAUTI_FIXTURE_PRESETS=1`, then run
`uv run python tests/obs/check_presets.py REPORT.presets.json`. The actual libobs
callbacks are exercised for all four presets and each Manual handoff, retained
hardware values, reopening, manual save/load, an external edit, an unknown ID,
and saved settings without a preset or with a retired ID, which reset to Medium. The UI
JSON includes effective visibility inherited from groups.
The standard OBS pytest suite includes this check. Slider mode 13 cycles through
all presets and Manual at two-second intervals during rendering.
