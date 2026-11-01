# 0008: Control defaults 60/50/50/50, 1280 px analysis, 30 fps, Performance

Status: Accepted; the effect strength defaults are superseded by [0020](0020-look-presets.md). Date: 2026-10-24.

## Context

Every new filter instance starts from the registered defaults. The four
strength controls, the analysis resolution, the requested analysis rate, the
quality mode, and the processing device all need a value that works without
reading documentation and that all measurements can share.

## Decision

| Setting | Default |
| --- | --- |
| Retouch | 60 % |
| Skin Tone, Micro Dodge & Burn, Smooth | 50 % each |
| Quality | Performance (384 px retouch, 256 px mask) |
| Analysis Resolution | 1280 px longer side |
| Target Frame Rate | 30 analyses per second |
| Processing Device | first accelerator of the build when the machine has it, else CPU ([0015](0015-default-device-follows-the-machine.md)) |

The defaults predate the measurements and are the configuration of every
throughput, continuity, and pixel measurement.

## Consequences

- Changing a default invalidates the comparability of the recorded
  measurements; a change is a minor release with a changelog entry and new
  measurements, as `docs/RELEASE.md` states.
- The Performance mode default keeps the common case within one OBS frame on
  Apple silicon and the measured NVIDIA card; High Quality costs about twice
  the inference time and is an explicit choice.
- 1280 px is the recommended entry in the resolution list; 960 and 1920 px are
  offered for slower and faster machines. Output resolution is never affected.

## Evidence

- `src/plugin/filter.cpp`, `defaults()`; the settings table in
  [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md).
- Fixture configuration of every development measurement: 1280 px analysis,
  Performance quality, controls 60/50/50/50.
