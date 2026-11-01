# 0002: The face detector runs on the CPU on every backend

Status: Accepted; the detector is MediaPipe BlazeFace ([0021](0021-mediapipe-detection-and-segmentation.md)). Date: October 2026.

## Context

The retouch and skin models run on the accelerated provider; the detector could as
well, and the ONNX Runtime providers support its operators.

## Decision

The detector session is always created with the CPU provider, regardless of the
selected processing device. No GPU detector option is offered.

## Consequences

- The detector runs at fixed cost on the CPU and never stalls the accelerator when a
  face leaves the picture.
- The detector's share of the worker time stays small compared with the two
  face models, so the steady-state loss is a few milliseconds per analysis.
- Provider profiles in `check_worker.py` verify the placement; a change needs a
  new comparison with face loss and recovery, not only steady-state throughput.

## Evidence

Measured during development before the first release; the figures in this record are the summary. The raw measurements are not part of the repository.
