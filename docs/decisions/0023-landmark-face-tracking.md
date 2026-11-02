# 0023: Follow the face through its landmarks

Status: Accepted for development. Date: 2026-11-02.

## Context

Each analysis searched the whole image for a face. BlazeFace misses a face that nearly fills
the image, two misses in a row ended the effect, and a landmark failure emptied the skin mask
at once. The crop also moved with the noise of the detector box, and even a one-pixel shift of
the crop changes the model outputs, which shows as flicker on a still face.

## Decision

- An established face is followed through FaceMesh: the landmarks of one analysis give the
  crop of the next, as in MediaPipe's landmarker. FaceMesh's score decides whether the face
  is still there. No model is added.
- The detector finds a face when none is followed, also with the image at 70% of its input,
  which brings a face that fills the image into the model's range. About once per second it
  checks for another face, which takes over only when it is separate, 1.5x larger, and seen by
  two checks in a row.
- A failure is bridged for up to 300 ms, or one analysis at a slow rate, by analyzing the
  current frame with the last region and landmarks. Holding the last output instead would
  freeze the whole source.
- After that the analysis reports no face and the continuity policy fades to the source. A
  landmark failure no longer empties the mask, so every exit runs through the same fade.
- A region that moved by at most 1% of the extent and about one degree is kept.
- A scene cut, another image size, or a pause of more than two seconds forgets the face.

## Consequences

- Analysis takes about 2 ms longer, since FaceMesh now runs before the retouch model instead of
  next to it. The detector mostly no longer runs.
- A low analysis rate falls back to the detector when the face has moved out of the crop, so it
  behaves no worse than a search on every analysis.
- A smaller face that is already followed stays selected until a larger one qualifies.
- During a bridge the feature cut-out may lag the face by up to 300 ms, and Eyes pauses. The
  segmenter still runs on the current frame, so an occluding hand is not treated as face skin.
