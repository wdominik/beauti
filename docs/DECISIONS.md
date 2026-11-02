# Decisions

The decisions that shape Beauti and why they stand. This page describes the current state; a
reversal changes its section, and history lives in Git. The how is in
[Architecture](ARCHITECTURE.md).

## Threads

The plugin has two threads of its own concern. The OBS graphics thread captures the source,
hands reduced frames to a worker, and composes the result. One worker thread per filter owns the
engine. Inference never runs in the render callback, because that would stall every source in the
scene, and the worker never touches OBS graphics objects.

Jobs and results pass through bounded slots guarded by one mutex, and a generation counter drops
work of an earlier source, size, or settings context. Shared state is listed in
[CONTRIBUTING.md](../CONTRIBUTING.md) under *Threads*.

The worker never calls OBS module functions; locale strings are copied when the filter is created.
libobs frees a module's locale table during deferred source destruction while a worker may still run.

## Models and their placement

- **Detection:** MediaPipe BlazeFace full range, on the CPU on every device. It runs at a fixed cost,
  never stalls the accelerator, and short range missed faces at half the fixture size.
- **Skin mask:** MediaPipe Multiclass Segmentation (256 px in both quality modes), face-skin class, with
  the FaceMesh eye, brow, and lip contours cut out. Landmarks alone could not exclude hair over the face
  or eyeglass frames. It runs next to the retouch model on the selected device.
- **Landmarks:** FaceMesh V2 and Iris stay on the CPU in every mode. Each further accelerator session
  delays the first processed frame, and the Iris graph splits into many small accelerator sections.
- **Mask preparation:** Erosion and feathering are shared shader passes on the OBS graphics abstraction,
  identical for every provider, with no interop and no provider-specific kernels.
- **Entry point:** `Engine::analyze()` is the only one and returns maps, the face region, and timings.
  There is no CPU compositing path. `src/effects/` is a separate reference library for the numerical tests
  and is not linked into the engine.

The TFLite models are converted to ONNX when they are prepared and verified against TFLite.

## Processing device

- **Default:** New filters start on the build's accelerator when the machine can run it, and otherwise
  on the CPU, which the status line says. The probe runs once per process when the first filter is
  created, without loading the provider, and writes nothing to the settings.
- **Failures are reported:** A selected device that fails shows in the status line and the log. It is
  never silently replaced by the CPU, because the user would see a slow filter without knowing why.
- **One Windows package** serves every machine. The CPU-only build is a compile and test variant.
- **CPU threads:** In CPU mode ONNX Runtime sizes its pools for the detector, retouch, and segmenter.
  CPU sessions next to an accelerator, and FaceMesh and Iris in every mode, use a quarter of the physical
  cores on Windows, at least two and at most four, and two on macOS. OBS keeps most of the CPU.

## NVIDIA TensorRT RTX

Windows uses ONNX Runtime's CPU distribution with NVIDIA's standalone TensorRT RTX execution provider and
its pinned archive. The EP is not forked. Packages do not contain NVIDIA's files: users copy them next to
the plugin ([installation guide](INSTALL.md)), and the plugin checks their sizes before loading them from
absolute paths with restricted search flags. OBS's DLL search policy stays untouched.

- **One policy for every supported GPU**, with no hardware table. New filters use Full Precision, which
  names the FP32 model files, not bit-exact arithmetic. FP16 is an explicit choice.
- **Requirements:** A compatible RTX GPU and a CUDA 13-capable driver.
- **Registration:** One synchronized registration lasts for the module's lifetime. CPU engines do not load
  NVIDIA libraries.
- **Caches** stay local, keyed by model, ONNX Runtime, and provider identities, and the provider validates
  them against its runtime and device. No GPU-specific cache ships.

## Apple Core AI

macOS 27 is the minimum, with no fallback for older systems. Core AI runs the retouch model on the Neural
Engine and the segmenter on the GPU, in parallel. ONNX Runtime remains for the CPU device, the detector,
and the landmarks.

The compute unit is set explicitly, and the retouch U-Net's upsampling is expressed as two fixed matrix
multiplications with the same interpolation, because otherwise the model alternates between the Neural
Engine and the GPU. Core AI has only a Swift interface, so a small adapter exposes it to C++ through a C
interface. Swift is used nowhere else. Conversion and placement are rechecked on every Core AI or macOS update
([Apple acceleration](APPLE-ACCELERATION.md)).

## Face tracking

An established face is followed through FaceMesh: the landmarks of one analysis give the crop of the
next, as in MediaPipe's landmarker, and FaceMesh's score decides whether the face is still there. Searching
every frame misses faces that nearly fill the image and lets detector noise move the crop.

- **Search:** The detector runs when no face is followed, also with the image at 70% of its input, which
  brings a face that fills the image into the model's range.
- **Another face** takes over only when it is separate, 1.5 times larger, and seen by two checks, about one
  second apart. A smaller face that is already followed stays selected until then.
- **Bridge:** A failure of up to 300 ms, or one analysis at a slow rate, analyzes the current frame with the
  last region and landmarks. Holding the last output instead would freeze the whole source. The segmenter
  still sees the current frame, so an occluding hand is not treated as face skin. Eyes fades out meanwhile.
- **Exit:** After that the analysis reports no face and the continuity policy fades to the source.
- **Steady region:** A region that moved by at most 1% of the extent and about one degree is kept, because
  even a one-pixel shift of the crop changes the model outputs and shows as flicker on a still face.
- **Square region:** The region may reach beyond the image, whose edge pixels repeat. Cutting it at the border
  would distort the face for the models and fade the effect at the image edge.
- **Reset:** A scene cut, another image size, or a pause of more than two seconds forgets the face.

A settled face runs FaceMesh next to the models on the region of the previous analysis, which saves its time.
A moving face or a slow rate runs it first, so that the models never see a face that left the region.

## Output continuity

One provider-independent policy holds the last complete, compatible result until a successor is accepted,
for a window derived from the observed intervals and at most five seconds from capture. A fixed expiry drops
a displayed result before the next one arrives and shows the unprocessed source during ordinary jitter.
Transitions to and from the source fade over 150 ms, control changes ramp over 150 ms with exact zero endpoints,
and recovery after a failure needs two valid results. The filter never applies old maps to a newer frame. The
contract is in [Output continuity](OUTPUT-CONTINUITY.md).

## Timing

The output shows the frame that was analyzed, so the analysis delay is the delay of the source.

- **Readback:** A capture is read back in the tick that issued it when the analysis can finish before the
  next capture, so its result arrives a tick earlier. A wait above 30% of the frame interval returns to the
  later readback for 5 seconds, doubling up to 60, so that it never waits repeatedly on a saturated GPU. The
  first such wait can cost a frame.
- **Automatic rate:** The default target is the canvas rate divided by a whole number, up to 60 FPS, because
  a target the analysis nearly reaches repeats frames at irregular moments, which looks worse than an even
  lower rate. The divisor grows only on evidence: results below 90% of the capture rate in two windows in a
  row, or an OBS frame time above 85% of its budget, the latter not below 15 FPS. It shrinks after 10 stable
  seconds when the analysis fits within 85% of the shorter interval. An increase that fails within 5 seconds
  doubles that wait, up to 120 seconds. A canvas above 40 FPS starts at half its rate. The OBS frame time
  includes everything else OBS renders, so a busy scene can also lower the rate.
- **Fixed rates** stay available and are followed exactly.

## Resource use

A live filter shares the machine with the encoder and often a game.

- ONNX Runtime pools sleep between analyses instead of spinning.
- The retouch model stops two seconds after Retouch reaches zero. With all effects at zero, captures and
  analysis stop after two seconds, and the output is the source itself.
- After three minutes without frames to analyze, the engine releases its models and loads them again on the next
  frame.
- Intermediate GPU fields use half floats. The tone histograms, which count up to 4096 samples, and the Micro
  Dodge & Burn statistics, which hold squares, stay in 32-bit floats.
- The finishing fields are at most 1536 px on their longer side. A larger region is computed at that size and
  sampled back, because its cost otherwise grows faster than its area.

Measured and not adopted: persistent threads for the mask (starting them costs well under 0.1 ms), one engine shared
by several filters (it would serialize their analyses), skipping unchanged frames (it helps still images only), and
temporal hysteresis in the mask (little effect, since most remaining change is real motion).

## Controls and presets

- **Presets:** Low (101), Medium (102), High (103), Very High (104), and Manual (0). IDs are saved identities,
  never list indices. New filters start at Medium. High is the reference look, and Very High removes fine texture
  more strongly with the same eye strength. Presets only configure existing stages.
- **Selecting a preset** writes its effect values as ordinary settings. Manual shows the effect sections and starts
  from those values. Processing settings are never part of a look. An effect value changed from outside while a
  preset is active switches the filter to Manual.
- **Finishing parameters** are ordinary saved settings ramped on the render thread, with no restart of inference,
  no checkable groups, and no algorithm selectors. Strength zero is the only off state.
- **Processing defaults:** Performance quality and 1280 px analysis, which keep the common case within one
  OBS frame; 960 and 1920 px are offered for slower and faster machines. The output resolution never changes.
- **Saved settings** are not migrated before 1.0. From 1.0 a change ships with an explicit migration.

## OBS version

OBS Studio 33.0.0 is the minimum on both platforms, built against pinned headers and SDK. Windows packages use the OBS 33
plugin layout only, which OBS 32 does not load; supporting both would mean two packages or an installer.

## Languages, dependencies, and tests

- **Languages:** C++20 for everything that ships, CMake presets as the build interface, and Python 3.14 for tooling only,
  as the package `beauti_tools`, plus the Swift adapter above. There is no shell, zsh, or PowerShell code, and
  installation is a documented copy.
- **Dependencies:** `buildspec.json` pins every build-time dependency by URL and SHA-256. Nothing comes from a moving branch, no
  system package manager is required, and the plugin needs no network at runtime
  ([Dependencies](DEPENDENCIES.md)).
- **Tests:** Catch2 for native tests and pytest for Python tests, whose markers name their prerequisites. The presets
  `macos-asan` and `macos-tsan` instrument every target; sanitized builds are never packaged, and continuity changes run the
  fault-injection suite under ThreadSanitizer ([Tests](TESTING.md)).
