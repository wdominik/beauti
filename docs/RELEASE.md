# Release process

Beauti has no public release yet. This page lists what a release requires and the
steps to produce one, so that the gaps are explicit.

## Release gates

| Gate | State | Owner |
| --- | --- | --- |
| macOS signing: Developer ID, notarization, stapling | Ad-hoc signing only | Project owner |
| Windows signing: Authenticode | Unsigned | Project owner |
| OBS compatibility | Built and checked against 33.0.0 on both platforms. Live camera and sustained encoding acceptance remain pending | Maintainers |
| MediaPipe models on Windows | Built and tested; camera review remains pending | Maintainers |
| Core AI cache after a host update | Core AI fails with `nilError` once the host's code signature changes; the adapter clears the process cache and loads again ([Apple acceleration](APPLE-ACCELERATION.md#adapter)). Reproduced only by re-signing a test binary: update OBS with Beauti installed and confirm the filter starts; report the Core AI defect to Apple | Maintainers |
| 4K at 60 FPS | The segmenter shares the GPU with OBS (macOS 4K render time 22–24 ms at 30 FPS); decide how to bound the analysis rate | Maintainers |
| Eyes on the CPU device | Without an accelerator, Eyes takes effect 0.8–1.3 s after it is enabled and fails the one-second transition check; accept or shorten | Maintainers |
| Model tooling on a final TensorFlow | Model preparation uses TensorFlow 2.22.0rc0 (no 2.21 build for Python 3.14); switch to the 2.22 release and confirm identical conversions | Maintainers |
| TensorRT RTX EP compatibility | ORT 1.31 is outside the EP's published matrix (1.25 to 1.30); locally validated, re-check on every ORT or EP update | Maintainers |
| Windows default on machines without NVIDIA or with an old driver | Hidden-device fallback passes; real missing/old-driver machines remain untested | Maintainers |
| User-installed NVIDIA runtime | Follow the [installation guide](INSTALL.md) on a real machine: without the files, with them after a restart, with a wrong version, and after an update | Maintainers |
| Camera, streaming, and recording acceptance on both platforms | Pending; see [PERFORMANCE-ACCEPTANCE.md](PERFORMANCE-ACCEPTANCE.md) | Project owner |
| CI green on the release commit | `.github/workflows/ci.yml`; the first run of `release.yml` is unverified | Maintainers |
| Native-speaker review of the locale catalogs (`data/locale/`) | Pending for all languages except English and German | Project owner |

## Steps

`.github/workflows/release.yml` builds both packages on GitHub and ends in a draft
release, which a person reads and publishes. Nothing is released by a push alone.

1. Update `VERSION` and move the `Unreleased` section of `CHANGELOG.md` to the new
   version with today's date. Keep the release notes in US English.
2. Refresh the provenance snapshot and verify it with the prepared models:
   `uv run beauti-update-snapshot && uv run beauti-verify-provenance --models`.
3. If `models/manifest.json` changed, publish the [model archive](#model-archive).
4. Run the checks that need hardware. macOS: `uv run beauti-verify-native
   dist/beauti.plugin --backend coreai` and the fixture checks from
   [TESTING.md](TESTING.md), including the continuity fault injection. Windows: copy the
   package and add the DLLs from `beauti-prepare-tensorrt` to the copy, as in the
   [installation guide](INSTALL.md); run `beauti-verify-native` for the TensorRT and CPU
   backends and the fixture checks on it. Also check the package without the DLLs.
5. Compare against a compatible reference with `tests/native/compare_workers.py`
   and the OBS fixture runner; investigate any change in detection boxes or maps
   before accepting it. See [protocol compatibility](TESTING.md#scope-and-limits)
   when comparing to an older release.
6. Try the packaging without a tag: `gh workflow run release.yml`, or **Run workflow**
   in the Actions tab. It builds the packages as artifacts and creates no release.
7. Tag the commit `vX.Y.Z` on `main` and push the tag. The workflow refuses a tag that
   differs from `VERSION` and a version without a changelog section. It builds, tests,
   and audits both packages (the Windows audit rejects a package with the NVIDIA
   runtime), then creates the draft with the archives, their `.sha256` files, the
   source archive, build provenance, and notes from the changelog.
8. Download a package from the draft, install it by the
   [installation guide](INSTALL.md), then publish the draft.

Signing is not part of the workflow yet. When it is added, sign Windows native binaries
before recording the final payload inventory, because signing changes their hashes. On
macOS, record the resource inventory before the bundle signature, then sign, notarize,
and staple. Verify the final signed bundle with the audit scripts, and do not repair a
failed inventory automatically.

## Model archive

Core AI conversion is not byte-reproducible, so release builds use the exact prepared
files from `.cache/models` instead of converting again. They come from a GitHub release
of their own, whose tag follows from `models/manifest.json`. The workflow checks every
file against the manifest. After changing the models, publish a new archive:

```sh
uv run beauti-model-archive pack dist/beauti-models.zip
gh release create "$(uv run beauti-model-archive tag)" dist/beauti-models.zip \
  --prerelease --latest=false --title "Prepared models" \
  --notes "Prepared models used to build Beauti, with their license texts."
```

## Versioning

`VERSION` is the single source of the product version; CMake, the bundle
manifest, the Info.plist, and the package names read it. Patch releases change
only fixes; minor releases may change defaults or add controls. From 1.0, a major
release changes saved-settings compatibility (the keys in the settings table in
[ARCHITECTURE.md](ARCHITECTURE.md) and the preset IDs). Before 1.0 saved settings
are not migrated; see [Presets](PRESETS.md#saved-settings-before-10). Effect
defaults are the default preset ([decision 0020](decisions/0020-look-presets.md));
processing defaults follow [decision 0008](decisions/0008-control-defaults.md).
