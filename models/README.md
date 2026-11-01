# Model preparation

The repository stores model identities and acquisition recipes. Large weights
and generated graphs are local build inputs under `.cache/models/`, ignored by
Git. `sources.json` records original URLs, byte counts, and SHA-256 hashes;
`manifest.json` records the prepared runtime files relative to that cache: ONNX graphs
for the CPU and NVIDIA devices and Core AI assets for macOS. Each bundle ships its
platform's subset.

With [uv](https://docs.astral.sh/uv/) installed:

```sh
uv sync --group dev --group models
uv run beauti-prepare-models
```

The sequence is original-download verification → PyTorch U-Net export (opset 17)
→ fixed-shape/ONNX preparation → FP16 variants → 384 px retouch variants → eye
models → MediaPipe detector and segmenter (TFLite to ONNX, verified against TFLite)
→ Core AI assets → prepared-file verification.
Scripts live in `tools/beauti_tools/models/`; copied export architecture code lives in
`third_party/modelscope_unet/`. `prepare.py --check` checks existing inputs and
outputs without downloads or conversion. The native build copies only files in
the prepared manifest into the plugin.

The FP16 and smaller-resolution variants are performance choices, not claims of
pixel equivalence to full-resolution FP32. The recorded hashes identify the actual
bytes and cause a changed upstream download to fail verification. Do not replace
those hashes merely to accept an unexplained mismatch.

Model preparation has been reproduced on Apple silicon with the pinned versions
in the `models` dependency group of `pyproject.toml`. Cross-platform serialization and execution
still require checks. A verified prepared-model source archive can supply the
runtime files without rerunning export; the normal native build needs only
those files, the runtime, and the SDK.

See [provenance](../docs/PROVENANCE.md). The retouch model's changes are described in
`licenses/ModelScope-NOTICE.txt`. The MediaPipe models are Apache-2.0; their notice is
`licenses/MediaPipe-NOTICE.txt`.

Eye models are copied without graph or weight changes by
`beauti_tools.models.prepare_eye_models`. FaceMesh V2 comes from yakhyo's ONNX
release; Iris comes from the exact nested PINTO archive member
`20_new_20211209/resources.tar.gz` → `saved_model_64x64/model_float32.onnx`.
Both acquired archives and final runtime bytes are hash-checked. The preparation
script only reads named regular members; it never extracts archive paths.
Apache-2.0 notices are in `licenses/FaceMesh-Apache-2.0.txt`,
`licenses/Iris-Apache-2.0.txt`, and `licenses/MediaPipe-NOTICE.txt`.

Core AI assets for macOS (`coreai/*.aimodel`) are produced by
`beauti_tools.models.prepare_coreai` from the PyTorch checkpoint (retouch) and the
prepared ONNX masks, with two numerically checked rewrites for the Neural Engine
and GPU; see [Apple acceleration](../docs/APPLE-ACCELERATION.md#model-assets). The
conversion needs macOS with the `models` group. Its output is not byte-reproducible,
so the step records the new hashes in `manifest.json`.
