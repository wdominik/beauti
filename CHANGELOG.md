# Changelog

All notable changes to Beauti are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). Dates are ISO 8601.

## [Unreleased]

### Fixed
- Removing a filter or quitting OBS could hang in rare cases.
- The target frame rate is now kept when it is not a divisor of the canvas rate; 25 FPS
  on a 30 FPS canvas used to analyze at 15 FPS.
- A persistent analysis failure now retries with a growing delay, up to 60 seconds,
  instead of rebuilding the models every 2 seconds. Changing the device, quality, or
  analysis resolution retries immediately.
- On macOS, a failed model load no longer clears the Core AI cache, except for the error
  that a changed host signature causes.
- **Tools → Beauti: Add Filter to Selected Source** now shows a message when no source is
  selected, instead of doing nothing.

## [0.10.0] - 2026-11-01

First public version of Beauti, an OBS Studio effect filter for real-time skin
retouching and finishing. Everything runs locally; nothing leaves the machine.

### Added
- **Face analysis:** finds the largest face in a source and runs local models on it:
  MediaPipe BlazeFace (face detection), a ModelScope retouching U-Net, MediaPipe
  Multiclass Segmentation for the skin mask, and MediaPipe FaceMesh, which keeps
  eyes, brows, and lips out of that mask.
- **Six stages composed on the GPU:** Retouch, Skin Tone, Local Color Smoothing,
  Micro Dodge & Burn, Smooth, and Eyes. Each is adjustable from 0 to 100%, and
  strength changes fade in over 150 ms.
- **Look presets:** Low, Medium, High, Very High, and Manual
  ([presets](docs/PRESETS.md)). New filters start at Medium. Manual starts from the
  last preset's values and exposes the detail controls of Smooth and Local Color
  Smoothing.
- **Eyes:** a subtle, bounded brightening of the iris and eye whites that keeps
  pupils, catchlights, and eye color, using FaceMesh and MediaPipe Iris
  ([Eyes](docs/EYES.md)).
- **Quality and analysis settings:** Performance (384 px retouch) and High Quality
  (512 px); analysis resolutions of 960, 1280, or 1920 px; an adjustable target
  frame rate. The output keeps the source resolution.
- **macOS 27 or later on Apple silicon:** Core AI runs the retouch model on the
  Neural Engine and the skin segmenter on the GPU
  ([Apple acceleration](docs/APPLE-ACCELERATION.md)).
- **Windows x64:** one package for every machine. With NVIDIA's runtime files added
  ([installation guide](docs/INSTALL.md)), TensorRT RTX runs the retouch model and the
  skin segmenter on NVIDIA RTX GPUs (Ampere or newer); otherwise the CPU is used. New
  filters start on the accelerator when it is available.
- **Output continuity:** a bounded last-good hold covers late or missed analyses,
  and the filter fades to the unprocessed image when the face is lost
  ([output continuity](docs/OUTPUT-CONTINUITY.md)).
- **Status line:** shows the analysis size and rate, and explains devices that
  are unavailable and sources that cannot be processed.
- **Prebuilt packages:** each release has macOS and Windows packages with checksums
  and build provenance.
- **Interface languages:** English, German, Spanish, Portuguese (Brazil), French,
  Russian, Japanese, Korean, and Chinese (Simplified and Traditional).

### Known limitations
- One face per source; SDR sources up to 4096 px per side.
- Saved settings are not migrated before 1.0.
