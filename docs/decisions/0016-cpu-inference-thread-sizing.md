# 0016: CPU inference uses ONNX Runtime's automatic thread sizing

Status: Accepted for development. Date: 2026-10-26.

## Context

Every CPU session previously used two intra-op threads, including both parallel
Retouch and skin-mask models in CPU mode. On the Windows validation host, CPU
High Quality delivered about 0.7 processed FPS, and a missed result could outlast
the bounded last-good hold. A focused 1/2/4/automatic comparison found a substantial
throughput improvement without changing model outputs or the continuity policy.

## Decision

When the engine's selected backend is CPU, pass zero as the intra-op thread count
to its three sessions and let ORT choose its normal pool size and placement. This
applies equally to explicit CPU selection and the existing unavailable-accelerator
default. It is one shared CPU policy, with no processor model table or new setting.

When the engine uses TensorRT RTX or Core ML, retain two intra-op threads for auxiliary
CPU sessions and one host thread for accelerator sessions. Retain the existing
parallel model scheduling, inter-op setting, graph options, and model selection.
Explicit four-thread sizing had no useful advantage over ORT automatic sizing
on the tested host, so no additional hardware-budget heuristic is introduced.

## Consequences

- CPU throughput improves about 33%/41% in the measured Performance/High Quality
  OBS runs, but remains far below real-time inference on this VM.
- The twelve candidate continuity checks pass without extending freezes or
  weakening recovery checks. Slower machines may still exceed the shared limits.
- More CPU parallel work competes with OBS and other applications. Two late
  render frames occurred across the two automatic High Quality comparison runs,
  versus zero with the reference; final-build spot checks also recorded occasional
  late frames. Sustained recording/streaming, multiple filters and other
  CPU topologies still need evaluation before release.
- Native Apple CPU performance and the required Apple ThreadSanitizer checks are
  open. Core ML's auxiliary CPU mask and detector retain their existing budgets.

## Evidence

- Measured during development before the first release; the figures in this record are the summary. The raw measurements are not part of the repository.
- [ORT thread management](https://onnxruntime.ai/docs/performance/tune-performance/threading.html).
