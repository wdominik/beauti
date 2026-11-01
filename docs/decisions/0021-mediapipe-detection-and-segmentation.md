# 0021: MediaPipe models for face detection and skin segmentation

Status: Accepted; the segmenter placement outside macOS is in [0022](0022-tensorrt-segmenter-and-auxiliary-threads.md). Date: 2026-10-27.

## Context

The mask must keep excluding hair over the face, eyeglass frames, eyes, brows, and
lips, so a mask built from landmarks alone is not enough. Google's MediaPipe models
cover this: BlazeFace full range and Multiclass Selfie Segmentation with background,
hair, body skin, face skin, clothes, and accessories. FaceMesh V2 and Iris also come
from MediaPipe.

## Decision

- BlazeFace full range (192 px letterbox) is the face detector on every device and
  runs on the CPU. Short range was rejected: it missed faces at half the fixture size.
- The skin mask is MediaPipe Multiclass Segmentation (256 px, both quality modes),
  face-skin class, with the FaceMesh eye, brow, and lip contours cut out. Without
  plausible landmarks the mask stays empty.
- FaceMesh is a required model; the mask and the eye correction share one FaceMesh
  result per frame.
- On macOS the segmenter runs through Core AI on the GPU.
- The analysis crop is 1.86x the BlazeFace box.
- The TFLite models are converted to ONNX at build time (tf2onnx) and verified
  against TFLite; the tooling pins a TensorFlow 2.22 release candidate because 2.21
  has no Python 3.14 build.

## Consequences

The Apache-2.0 models are ModelScope retouch and the four MediaPipe models. The ears
count as skin. On an M4 Pro the analysis takes 13.4 ms in Performance and 21.4 ms in
High Quality; the GPU segmenter raises the OBS render time at 4K to 22–24 ms.

## Evidence

[Apple acceleration](../APPLE-ACCELERATION.md). BlazeFace full range found 23 of 24 face
variants (original, 50% and 30% size, ±30° roll, dark); short range missed three of
four at 50% size. Converted graphs against TFLite: detector `7.6e-5`, segmenter
`2.0e-5`.
