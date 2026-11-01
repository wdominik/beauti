# 0013: Catch2 for native tests, pytest for Python tests, sanitizer presets

Status: Accepted. Date: 2026-10-26.

## Context

The native tests were three executables with a hand-written `require()` that
printed "Contract failed" without values or names. Python checks were scripts
with their own argument parsing and no shared runner; CI could not select the
subset that needs no build output. The thread model
([0001](0001-inference-off-the-graphics-thread.md)) had never run under a
sanitizer.

## Decision

- Catch2 3.16.1 (amalgamated, pinned in `buildspec.json`) compiles into one
  `beauti-tests` executable; CTest keeps the entries `continuity`,
  `pixel-quantize`, and `provider-policy` through tags. doctest was the
  alternative; Catch2 was chosen for generators, sections, and matchers.
- pytest 9 is the only Python runner. Markers `needs_reference`, `needs_bundle`,
  and `needs_gpu` name the prerequisites, which come from `BEAUTI_*` environment
  variables described in `tests/conftest.py`; missing ones skip with a reason.
  The `check_*.py` scripts expose `run()` and keep their command lines.
- `BEAUTI_SANITIZER` with the presets `macos-asan` (Address plus Undefined
  Behavior) and `macos-tsan` instruments every target. Sanitized builds are never
  packaged. Continuity changes run the fault-injection suite under TSan.

## Consequences

- The first TSan run found the locale data race behind 0001's module-function rule.
- CI runs `pytest -m "not needs_bundle and not needs_gpu"`; bundle and GPU
  checks run locally and on the Windows host (`docs/TESTING.md`).
- The amalgamated Catch2 source is compiled once per build directory into a
  static library with warnings-as-errors off; the project targets keep theirs.

## Evidence

- `CHANGELOG.md`, *Added* and *Fixed*.
- [`docs/TESTING.md`](../TESTING.md), *Running the tests*; `CONTRIBUTING.md`, *Testing*.
