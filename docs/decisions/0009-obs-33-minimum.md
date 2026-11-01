# 0009: OBS Studio 33.0.0 is the minimum host version

Status: Accepted. Date: 2026-10-25.

## Context

OBS 33.0.0 (2026-10-23) reworked plugin loading and defines a Windows plugin
folder layout (`plugins\beauti\beauti.dll` beside `data\`); OBS 34 is announced
to stop loading the layout of OBS 32 and earlier. Supporting both layouts would
mean two Windows packages or an installer that detects the host version.

## Decision

Build against the pinned OBS 33.0.0 headers and SDK on both platforms; ship the
OBS 33 Windows layout only. OBS 32 and earlier are not supported.

## Consequences

- The Windows package is not loaded by OBS 32; the README states the
  requirement, and the bundle audit checks the layout.
- The macOS bundle layout is the same for every OBS version; validation is
  against 33.0.0 only.
- OBS 33 frame timing exposed a margin in the continuity hold; the hold formula
  in [0006](0006-bounded-last-good-hold-and-150ms-transitions.md) accounts for it.
- Native Windows validation with this layout is pending (`docs/RELEASE.md`, gates).

## Evidence

- `CHANGELOG.md`, *Changed*; `buildspec.json`, OBS entries.
- [`docs/DEPENDENCIES.md`](../DEPENDENCIES.md), OBS entries.
