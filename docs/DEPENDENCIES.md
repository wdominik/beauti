# Dependencies

Inventory of everything Beauti depends on, by scope and platform, with the pinned
and the current upstream version. Checked on 2026-10-25 (Catch2 and pytest on
2026-10-26) against PyPI, the GitHub release pages, endoflife.date, and the model
sources. Pins live in `buildspec.json` (native), `pyproject.toml` with `uv.lock`
(Python), `.github/workflows/ci.yml` (CI), and `models/sources.json` (weights).

## Runtime components

| Component | Platform | Pinned | Latest | Role |
| --- | --- | --- | --- | --- |
| ONNX Runtime library | macOS arm64 | 1.31.0 (official release archive) | 1.31.0 | CPU inference: detector, eye models, CPU device |
| Core AI framework | macOS 27 | system framework | macOS 27.0 | Retouch and skin-mask inference on the Neural Engine and GPU |
| ONNX Runtime library | Windows x64 CPU and NVIDIA | 1.31.0 (CPU archive) | 1.31.0 | Shared inference host and CPU provider |
| TensorRT RTX standalone EP | Windows NVIDIA, installed by the user | 0.5.0 cu13 | 0.5.0 | Dynamically registered NVIDIA provider |
| TensorRT RTX runtime, ONNX parser, plugins | Windows NVIDIA, installed by the user | 1.7 (from the EP archive) | 1.7 | Three runtime DLLs accompanying the EP |
| Retouch U-Net weights | all | ModelScope `cv_unet_skin_retouching_torch`, hash-pinned | unchanged upstream | Apache-2.0 |
| Skin segmentation | all | MediaPipe Multiclass Selfie Segmentation 256, hash-pinned TFLite | unchanged upstream | Apache-2.0 |
| Face detector | all | MediaPipe BlazeFace full range, hash-pinned TFLite | unchanged upstream | Apache-2.0 |
| Face landmarks | all | MediaPipe FaceMesh V2 (yakhyo ONNX conversion), hash-pinned | unchanged upstream | Apache-2.0 |
| Iris landmarks | all | MediaPipe Iris (PINTO ONNX conversion), hash-pinned | unchanged upstream | Apache-2.0 |
| OBS libobs | all | linked against the installed OBS at run time | 33.0.0 | Host API; not bundled |

The NVIDIA runtime is the four-DLL set in `buildspec.json` (`tensorrt-rtx`). Packages
do not contain it; users install it from NVIDIA ([installation guide](INSTALL.md)).
No CUDA Toolkit or cuDNN is required. Driver-provided `nvcuda.dll`/`nvml.dll` and the
current x64 VC++ redistributable remain external prerequisites. See
[decision 0017](decisions/0017-tensorrt-rtx.md).

The upstream EP 0.5.0 matrix lists ORT 1.25–1.30. ORT 1.31 is locally validated
here, beyond that published matrix; this is not an upstream compatibility promise.
[Integration guide](https://nvidia.github.io/TensorRT-RTX-EP-ABI/integration-guide.html),
[release matrix](https://github.com/NVIDIA/TensorRT-RTX-EP-ABI/tree/v0.5.0), and
[runtime prerequisites](https://nvidia.github.io/TensorRT-RTX-EP-ABI/prerequisites.html).

## Build-time only

| Component | Platform | Pinned | Latest | Role |
| --- | --- | --- | --- | --- |
| OBS Studio headers and app/SDK | all | 33.0.0 | 33.0.0 (2026-10-23) | `libobs` headers; macOS links the installed app, Windows an import library from the official build |
| SIMDe | macOS | 0.8.2 | 0.8.2 (2024) | SSE intrinsics in libobs headers on arm64 |
| Catch2 | all (tests only) | 3.16.1 (amalgamated header and source) | 3.16.1 (2026-10-08) | Native test framework; compiled into `beauti-tests`, never shipped |
| ONNX Runtime C API headers | all | API level 31 (1.31.0) in `third_party/` | 1.31.0 headers | Compile-time API, matching the pinned runtime |
| ModelScope U-Net source | model export | hash-pinned copy in `third_party/` | unchanged upstream | PyTorch export only |
| CMake | all | 4.4.4 (`cmake_minimum_required 3.25...4.4`) | 4.4.4 | Build |
| Ninja | macOS | 1.13.2 | 1.13.2 | Generator |
| Visual Studio | Windows | 2026, 18.11.2 on the validation host (generator `Visual Studio 18 2026`) | 18.11.2, checked 2026-10-26 | MSVC toolchain |
| Xcode Command Line Tools | macOS | macOS 27 SDK, Apple Clang 21, Swift 6.4 (host) | Xcode 27 | Compilers (C++, Swift adapter); deployment target macOS 27 |
| Python interpreter | all | 3.14 (`.python-version`, 3.14.9) | 3.15.1; 3.14 is the active line | Tools only |
| hatchling | all | 1.32.4 | 1.32.4 | Build backend of the tools package |
| uv | all | 0.13.5 (host) | 0.13.5 | Environment and lockfile |

## Developer tools (`dev` group)

| Package | Pinned | Latest |
| --- | --- | --- |
| clang-format | 23.1.3 | 23.1.3 |
| ruff | 0.17.2 | 0.17.2 |
| mypy | 2.4.0 | 2.4.0 |
| pre-commit | 4.7.0 | 4.7.0 |
| actionlint-py | 1.7.12.25 | 1.7.12.25 |
| pefile | 2024.8.26 | 2024.8.26 |
| cmake, ninja (PyPI) | 4.4.4, 1.13.2 | 4.4.4, 1.13.2 |

## Test libraries (`test` group)

| Package | Pinned | Latest | Note |
| --- | --- | --- | --- |
| pytest | 9.1.1 | 9.1.1 | The single Python test runner |
| numpy | 2.4.6 | 2.5.4 | Held below 2.5 by coreai-torch (models group) |
| opencv-python-headless | 5.0.0.93 | 5.0.0.93 | Used only by the reference tests (color conversion, blur, resize) |

## Model preparation (`models` group)

| Package | Pinned | Latest |
| --- | --- | --- |
| torch | 2.14.1 | 2.14.1 |
| onnx | 1.23.2 | 1.23.2 |
| onnxruntime | 1.31.0 | 1.31.0 |
| protobuf | 7.36.2 | 7.36.2 |
| ml-dtypes | 0.6.0 | 0.6.0 |
| coreai-core | 1.0.0b3 | 1.0.0b3 |
| coreai-torch | 0.4.3 (Apple; validated through PyTorch 2.13, outputs checked with 2.14.1) | 0.4.3 |
| coreai-onnx | 1.2.1 (third party, BSD-3-Clause) | 1.2.1 |
| tensorflow | 2.22.0rc0 (2.21 has no Python 3.14 build; conversions are identical, see the MediaPipe report) | 2.22.0rc0 |
| tf2onnx | 1.17.0 | 1.17.0 |

The Core AI packages, TensorFlow, and tf2onnx run at build time only, to produce the
ONNX and `.aimodel` assets; the plugin ships none of their code. Move TensorFlow to
the 2.22 release once it is published.

## CI

| Component | Pinned | Latest |
| --- | --- | --- |
| actions/checkout | v7.0.1 (SHA) | v7.0.1 |
| astral-sh/setup-uv | v10.3.2 (SHA) | v10.3.2 |
| actions/cache | v6.1.0 (SHA) | v6.1.0 |
| actions/upload-artifact | v7.0.2 (SHA) | v7.0.2 |
| Runners | `ubuntu-latest`, `xcode-27` (macOS 27), `windows-2025` | current images |

Dependabot proposes action and Python updates weekly; native pins in
`buildspec.json` are updated by hand with the procedure below.

## Assessment (updated 2026-10-26)

Everything in the `dev`, `models`, and CI tables is current. The items that need
a decision:

1. **OBS Studio 33.0.0** (2026-10-23) is pinned and the Windows install layout
   follows its plugin-loading overhaul (`plugins\beauti\beauti.dll` with `data\`
   beside it; OBS 32 and earlier are not supported). Native Windows builds, D3D11
   finishing, and fixture processing pass; real-camera acceptance
   is open ([RELEASE.md](RELEASE.md)).
2. **ONNX Runtime 1.31.0 on both platforms** is pinned. Windows NVIDIA uses the
   standalone TensorRT RTX EP 0.5.0 on the same CPU archive; the driver floor is
   CUDA 13 ([decision 0017](decisions/0017-tensorrt-rtx.md)).
3. **Python 3.14** is the pinned line (3.12 was in security-only maintenance). 3.15
   waits for torch wheels.
4. **opencv-python-headless 5.0** is pinned; the 20 numerical reference tests and the
   finishing-pixel check pass unchanged.
5. **Model weights** are verified by hash; upstream bytes are unchanged today.

## Update procedure

1. Change the pin (`buildspec.json`, `pyproject.toml`, workflow SHA with its version
   comment, or `models/sources.json`), run `uv lock` for Python changes.
2. `uv run cmake --workflow --preset macos-ci`, `uv run beauti-build`,
   `uv run beauti-verify-native`, and on Windows the CPU and TensorRT builds with
   `beauti-prepare-windows`/`beauti-prepare-tensorrt`.
3. Run the comparison tests against the previous bundle (`compare_workers.py --exact`,
   `beauti-gpu-check` with `check_finishing_pixels.py`, the libobs fixture runner
   and the continuity suite) as described in `docs/TESTING.md`; a runtime or model
   change needs the camera acceptance from `docs/PERFORMANCE-ACCEPTANCE.md`.
4. Refresh the provenance snapshot, add a changelog line, update this page.
