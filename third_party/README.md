# Third-party source

- `modelscope_unet/unet_deploy.py` and `weights_init.py` are unchanged ModelScope
  skin-retouching architecture files. Original URLs and hashes are in
  `models/sources.json`; the Apache 2.0 notice is in `licenses/`.
- `onnxruntime/include/` contains Microsoft ONNX Runtime C API headers. The
  native engine uses API level 31 (ONNX Runtime 1.31.0 on both platforms). MIT and third-party notices are in `licenses/`.

OBS and SIMDe headers are acquired into `.cache/sdk/` by `cmake/Dependencies.cmake`
(pins in `buildspec.json`) rather than stored here. Prepared runtime binaries record their identity in each
bundle's `bundle.json`. See [provenance](../docs/PROVENANCE.md).
