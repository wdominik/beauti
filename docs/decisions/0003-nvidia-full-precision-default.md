# 0003: One NVIDIA policy: Full Precision default

Status: Accepted. Date: October 2026.

## Context

The Windows package offers NVIDIA Acceleration (FP16 retouch graph) and NVIDIA
Acceleration (Full Precision). Per-GPU lookups would be possible but would need a
maintained hardware table.

## Decision

- New filters default to Full Precision when NVIDIA acceleration is available
  ([0015](0015-default-device-follows-the-machine.md)). FP16 remains an explicit
  choice; saved selections are preserved.
- There is one policy for all NVIDIA GPUs; no GPU-model-specific selection.

## Consequences

"Full Precision" identifies the FP32 model files, not bit-exact arithmetic
([0017](0017-tensorrt-rtx.md)).

## Evidence

[`docs/PERFORMANCE-ACCEPTANCE.md`](../PERFORMANCE-ACCEPTANCE.md), *Complexity and cross-platform policy*.
