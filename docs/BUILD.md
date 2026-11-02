# Building Beauti

Commands run from the repository root. Python is used for build orchestration,
model export, and tests; it is not an OBS runtime dependency. macOS and Windows
share the C++ engine, OBS module, effects, model manifest, and source layout.

## macOS (Apple silicon)

Install the Xcode Command Line Tools with the macOS 27 SDK (they include Swift for
the Core AI adapter), [uv](https://docs.astral.sh/uv/), and OBS Studio 33.0.0 in
`/Applications/OBS.app`. uv installs Python 3.14 (`.python-version`), CMake, Ninja,
the formatters, and the tools package from the lockfile. The deployment target is
macOS 27, the first version with Core AI.

```sh
uv sync --group dev                       # tools, CMake, Ninja, formatters, test libraries
uv sync --group dev --group models        # add PyTorch/ONNX for model preparation
uv run beauti-prepare-models
uv run cmake --workflow --preset macos    # or put .venv/bin on PATH and call cmake directly
uv run beauti-build
uv run beauti-verify-native dist/beauti.plugin --backend cpu
uv run beauti-package dist/beauti.plugin
```

`uv run` executes a command inside the project environment; `uv sync --locked`
refuses to deviate from `uv.lock`, which is what CI uses.

Keep the checkout outside iCloud-synced folders (Desktop, Documents). Sync can set
the macOS hidden flag on files inside `.venv`, and Python 3.12.15 and later ignore
hidden `.pth` files, so `import beauti_tools` fails with `ModuleNotFoundError`
until `chflags -R nohidden .venv` is run. Sync can also add Finder metadata that
`codesign --verify` rejects; the build therefore signs in a temporary directory.

`cmake --workflow --preset macos` configures, compiles, and runs CTest without any
Python involvement; the `macos-ci`, `windows-ci`, and `windows-cpu-ci` presets are the
same builds with warnings treated as errors and are what CI runs; `macos-asan` and
`macos-tsan` build the same tree with AddressSanitizer plus UndefinedBehaviorSanitizer
or ThreadSanitizer into `build/macos-asan` and `build/macos-tsan` (see
[Testing](TESTING.md)) and are never packaged. On the first run, `cmake/Dependencies.cmake` downloads the
pinned archives from `buildspec.json` into `.cache/` and verifies their SHA-256:
OBS headers (`libobs/`, `frontend/api/`) from the release source archive, SIMDe
v0.8.2, the official ONNX Runtime 1.31.0 arm64 release, and the Catch2 3.16.1
sources for the native tests. The plugin links
against the installed OBS application; when `/Applications/OBS.app` is missing
(for example on a CI runner), the pinned OBS release image is downloaded, mounted,
and copied to `.cache/obs-app/OBS.app`. A version mismatch between the installed
app and the pinned headers is reported as a warning.

Model preparation downloads only the recorded original weights, validates hashes,
exports and transforms them, and validates every prepared file in `models/manifest.json`. See
[model preparation](../models/README.md) for reproducibility limits.
`--check` on `beauti-prepare-models` performs checks without downloads or exports.

`beauti-build` runs the same preset, then stages the bundle: models from
the manifest, runtime library, license notices, corresponding source, and the
ad-hoc signature. It does not modify the installed OBS plugin or launch OBS.
The previous staged bundle is retained as `<name>.previous` (one copy). Optional arguments
include `--cmake PATH`, `--obs-app PATH`, `--ort-library PATH` (to build against a
different ONNX Runtime dylib), `--build-dir PATH`, `--output PATH`, and
`--build-only`. An offline build needs the prepared `.cache/` dependencies.

## Windows x64

Install [uv](https://docs.astral.sh/uv/), OBS 33.0.0, and Visual Studio 2026 with the
C++ desktop workload and the current x64 Microsoft Visual C++ Redistributable;
uv provides Python 3.14 and CMake 4.4 from the lockfile. The
configured CMake generator is `Visual Studio 18 2026`. From a developer command
prompt:

```powershell
uv sync --locked --group dev
uv run beauti-verify-provenance --models
uv run beauti-prepare-windows --device cpu
uv run cmake --workflow --preset windows-cpu-ci
```

A Windows transfer includes every prepared model in `models/manifest.json`; copy it
into `.cache/models/` and do not prepare models again. The Core AI assets in it are
used on macOS only but are part of the provenance check. For a source-only checkout,
follow the model-preparation instructions in models/README.md first; on Windows the
preparation skips the Core AI step and keeps the recorded assets.

Use the `obs_sdk` and `ort_root` paths in `.cache/windows/prepared.json`:

```powershell
uv run beauti-build --platform windows --cpu-only --build-dir build/windows-cpu --output dist/cpu/beauti --obs-sdk .cache/windows/sdk --ort-root .cache/windows/ort_cpu/onnxruntime-win-x64-1.31.0
uv run beauti-verify-native dist/cpu/beauti --backend cpu
```

The NVIDIA build uses the same ONNX Runtime CPU archive and omits `--cpu-only`. It
loads NVIDIA's standalone TensorRT RTX execution-provider distribution at run time,
but the package does not contain it: users install it
([installation guide](INSTALL.md)), and `beauti-audit-windows-bundle` rejects a package
that contains it. `buildspec.json` pins the official EP 0.5.0 archive (TensorRT RTX
1.7, CUDA 13), its SHA-256, and its four runtime DLLs with their sizes and hashes. The
plugin compares the sizes before loading. No CUDA Toolkit, cuDNN, Windows ML, Windows
App SDK, or Python is required by the installed plugin.

For local tests, `beauti-prepare-tensorrt` stages those four DLLs and all upstream
notices into `.cache/windows/tensorrt-runtime`; copy the DLLs into a copy of the
bundle, never into the bundle you package. Pin updates validate a complete replacement
before removing obsolete files. Failed preparation preserves the previous runtime.
`--download-only` on `beauti-prepare-windows` permits archive preparation and PE
inspection on another OS; MSVC import-library generation still requires Windows.

```powershell
uv run beauti-prepare-windows --device tensorrt
uv run beauti-prepare-tensorrt
uv run cmake --workflow --preset windows-ci
uv run beauti-build --platform windows --obs-sdk .cache/windows/sdk --ort-root .cache/windows/ort_cpu/onnxruntime-win-x64-1.31.0
uv run beauti-audit-windows-bundle dist/tensorrt/beauti
Copy-Item dist/tensorrt/beauti dist/tensorrt/test -Recurse
Copy-Item .cache/windows/tensorrt-runtime/*.dll dist/tensorrt/test
uv run beauti-verify-native dist/tensorrt/test --backend tensorrt
uv run beauti-verify-native dist/tensorrt/test --backend tensorrt-fp32
uv run beauti-verify-native dist/tensorrt/test --backend cpu
```

GPU execution needs a supported NVIDIA RTX GPU (EP targets SM 8.6, 8.9, or
12.0+) and a CUDA 13-capable driver. Use a current supported driver; the EP's
Windows compatibility floor is 580.88, subject to GPU/runtime requirements.
CPU fallback remains available when the machine has no supported accelerator or the
NVIDIA files are missing. An explicitly
selected unavailable accelerator reports an error. Backend setting IDs 3 and 4
remain unchanged. TensorRT RTX runs the retouch model and the skin segmenter; face
detection and the landmarks run on the CPU. See
[TensorRT RTX](DECISIONS.md#nvidia-tensorrt-rtx) for the startup cache and validation
limits, and [models and their placement](DECISIONS.md#models-and-their-placement)
for the MediaPipe models and their placement.

These commands stage development bundles. Run the face, graphics, continuity,
and selection checks in [TESTING.md](TESTING.md); the smoke checks above only
exercise synthetic no-face frames. Packaging and release gates are in
[RELEASE.md](RELEASE.md). The CPU-only build is a test variant.

### Incremental development

Add `--build-only` to configure, compile, and run CTest without copying models or
source into a new package. A compile-only TensorRT build needs the ONNX Runtime CPU
SDK.

```powershell
uv run beauti-build --platform windows --cpu-only --build-only --obs-sdk .cache/windows/sdk --ort-root .cache/windows/ort_cpu/onnxruntime-win-x64-1.31.0
uv run beauti-build --platform windows --build-only --obs-sdk .cache/windows/sdk --ort-root .cache/windows/ort_cpu/onnxruntime-win-x64-1.31.0
```

Default build directories are `build/windows-cpu` and `build/windows-tensorrt`;
default staged bundles are `dist/cpu/beauti` and `dist/tensorrt/beauti`. The TensorRT RTX
bundle is the Windows development package and also serves machines without an NVIDIA
GPU (see [decisions](DECISIONS.md#processing-device)); the CPU-only build exercises the code paths without TensorRT in
CI and for tests. The Windows
CMake presets use the same build directories. The Python orchestrator selects
`windows-cpu` or `windows` instead of maintaining separate generator, architecture,
and build-option defaults. Explicit SDK, runtime, and output paths still work.
When a pin changes, cached Windows ONNX import-library and DLL paths under the
project's `.cache/` follow the new pin; external custom paths remain unchanged.
The orchestrator prefers CMake beside the active Python interpreter, validates
the minimum version, and uses CTest from that same installation. Missing Windows
SDK/import libraries or packaging runtimes are rejected before compilation.
Tool versions are recorded in `build-environment.json` and the staged manifest.

Every new bundle records `payload_sha256` in `bundle.json`: models, resources,
notices, corresponding source, and Windows native binaries. Missing, modified,
or additional files fail the audit. Required models and shader/locale resources
are also compared with the included source manifests, so an incomplete staging
directory cannot become valid merely by recording its current contents. Audits
never repair files or regenerate the inventory. Older development bundles without
this inventory must be restaged before using the current packaging tools.

Publication copies and audits the complete candidate beside the destination before
renaming the current bundle. One `<name>.previous` bundle is kept for comparison;
a failed final rename restores the current bundle. Staging paths inherit the
destination parent's permissions, including Windows ACLs. Copy or audit failures
leave both the current bundle and its previous copy untouched.

ZIP creation similarly prepares the archive and SHA-256 file beside their final
paths. CRC verification and hashing finish before either existing file is moved.
Rename failures restore the prior pair. These are same-volume rename operations,
not a power-loss-atomic transaction across multiple paths; if rollback itself fails
(for example, because of a file lock), the `.staging-*` directory retains recovery
copies. Do not delete it until the previous output has been recovered.

The packager audits its temporary archive copy and recalculates Windows report
identity from that copy, including binaries, models, both GPU effects, the cache
key, and every locale. Its README reports the current match and the report's
separate pass/fail result. Matching bytes do not mean passing tests; rebuilding
does not rerun pixel, camera, or performance checks. TensorRT requirements are read
from the bundle's included buildspec, not from a potentially newer checkout.

On macOS, the shared inventory is written before signing. The three native
binaries and generated code signature are excluded from its hashes because
signing changes them; their presence and strict native code signatures are
required instead. Models, resources, source, notices, and Info.plist remain
covered by the inventory. The shared checks have portable regression tests;
native signing/audit validation of this change still needs an Apple host.
Version choices are recorded in [DEPENDENCIES.md](DEPENDENCIES.md).

## Isolated comparison builds

For isolated A/B builds, do not restore source using preserved older timestamps
and assume an incremental build recompiles it. Use separate build directories,
force a rebuild, or update restored file modification times. Keep each variant's
binary hashes with its measurements; source text alone does not identify the
code linked into a previously built executable.

## Reference library and source archives

```sh
uv run beauti-build-reference
uv run beauti-export-source dist/Beauti-Source.zip
```

The reference library builds from `src/effects/` and is used by the CPU numerical
tests. Source archives include build and model-preparation recipes, docs, and
notices, but omit generated outputs and weights by default. `--include-models`
adds the eight verified runtime files under `.cache/models/`.
A source archive with prepared models can go straight to runtime preparation and
native build. Each plugin bundle also includes its corresponding source.
