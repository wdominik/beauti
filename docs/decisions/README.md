# Architecture decisions

One page per decision that shapes the product. Each record states the context,
the decision, its consequences, and the evidence that justified it (Nygard's ADR
format). Records are not edited after acceptance; a reversal gets a new record that
supersedes the old one and links back.

Adding a record: copy the headings of an existing page, number it next in
sequence, add it to the table below, and link it from the changelog entry of the
change that implements it. The record states the measurements it rests on.

| No. | Decision | Date | Status |
| --- | --- | --- | --- |
| [0001](0001-inference-off-the-graphics-thread.md) | Inference runs on one worker thread; the graphics thread only composes | 2026-10-26 | Accepted |
| [0002](0002-face-detector-on-cpu.md) | The face detector runs on the CPU on every backend | 2026-10 | Accepted; detector model per 0021 |
| [0003](0003-nvidia-full-precision-default.md) | One NVIDIA policy: Full Precision default | 2026-10 | Accepted |
| [0004](0004-apple-core-ml-default-and-cpu-skin-mask.md) | Apple default is regular Core ML; the Performance skin mask runs on the CPU | 2026-10-25 | Superseded by 0018 |
| [0005](0005-shared-gpu-mask-preparation.md) | Mask erosion and feathering run in the OBS shader for every provider | 2026-10-25 | Accepted |
| [0006](0006-bounded-last-good-hold-and-150ms-transitions.md) | Bounded last-good hold, one-miss bridging, 150 ms transitions | 2026-10-25 | Accepted |
| [0007](0007-report-provider-failures.md) | Provider failures are reported, never silently replaced by CPU | 2026-10-24 | Accepted |
| [0008](0008-control-defaults.md) | Control defaults 60/50/50/50, 1280 px analysis, 30 fps, Performance | 2026-10-24 | Effect defaults superseded by 0020 |
| [0009](0009-obs-33-minimum.md) | OBS Studio 33.0.0 is the minimum host version | 2026-10-25 | Accepted |
| [0010](0010-two-languages-no-shell.md) | Two languages: C++ for the product, Python for the tooling, no shell scripts | 2026-10-25 | Accepted; Swift adapter added by 0018 |
| [0011](0011-hash-pinned-dependencies.md) | Every build-time dependency is pinned by URL and SHA-256 in `buildspec.json` | 2026-10-25 | Accepted |
| [0012](0012-single-analysis-entry-point.md) | The engine has one entry point; the CPU references live in a separate library | 2026-10-25 | Accepted |
| [0013](0013-test-frameworks-and-sanitizers.md) | Catch2 for native tests, pytest for Python tests, sanitizer presets | 2026-10-26 | Accepted |
| [0015](0015-default-device-follows-the-machine.md) | The default device follows the machine: accelerator when present, otherwise CPU | 2026-10-26 | Accepted |
| [0016](0016-cpu-inference-thread-sizing.md) | CPU inference uses ORT automatic thread sizing; auxiliary CPU budgets stay bounded | 2026-10-26 | Accepted for development; Windows auxiliary budget superseded by 0022 |
| [0017](0017-tensorrt-rtx.md) | Direct TensorRT RTX with four DLLs and bounded pooling exception | 2026-10-26 | Development; camera acceptance pending |
| [0018](0018-core-ai-replaces-core-ml.md) | Core AI replaces Core ML on macOS; macOS 27 is the minimum | 2026-10-27 | Accepted |
| [0019](0019-configurable-finishing.md) | Adjustable Smooth parameters and Local Color Smoothing, without mode switches | 2026-10-27 | Accepted for development |
| [0020](0020-look-presets.md) | Look presets Low/Medium/High/Very High and a Manual mode that starts from the last preset | 2026-10-27 | Accepted for development |
| [0021](0021-mediapipe-detection-and-segmentation.md) | MediaPipe BlazeFace and Multiclass Segmentation for detection and the skin mask | 2026-10-27 | Accepted; segmenter placement outside macOS superseded by 0022 |
| [0022](0022-tensorrt-segmenter-and-auxiliary-threads.md) | The segmenter runs on TensorRT RTX; auxiliary CPU threads follow the cores | 2026-10-28 | Accepted for development |
| [0023](0023-landmark-face-tracking.md) | Follow the face through its landmarks, with a short bridge |
| [0024](0024-readback-timing-and-automatic-rate.md) | Read captures back at once when it is cheap; automatic analysis rate |
