# 0011: Every build-time dependency is pinned by URL and SHA-256 in `buildspec.json`

Status: Accepted. Date: 2026-10-25.

## Context

The build needs OBS headers and SDK, SIMDe, ONNX Runtime per platform, the
NVIDIA provider archive on Windows, and Catch2 for the tests. Options were
Git submodules, CMake `FetchContent` from upstream repositories, package
managers (Homebrew, vcpkg), or a project-owned manifest of release archives.

## Decision

`buildspec.json` lists every dependency with version, URL, SHA-256, and archive
root. `cmake/Dependencies.cmake` downloads into `.cache/`, verifies the hash, and
extracts with path checks; the Windows SDK preparation reads the same file.
Official release archives are used (the macOS ONNX Runtime from the GitHub
release, not the pip wheel). Nothing is fetched from a moving branch.

## Consequences

- A clean checkout configures and builds with `cmake --preset macos` without
  Python; CI caches `.cache/downloads` keyed by the manifest hash.
- Updating a dependency is one edit with a recorded hash, reviewed through the
  procedure in `docs/DEPENDENCIES.md`; Dependabot covers the Actions and Python
  pins, the native pins are updated by hand.
- No system package manager is required, and no network access exists at
  runtime.

## Evidence

- `CHANGELOG.md`, *Added*; `buildspec.json`.
- [`docs/DEPENDENCIES.md`](../DEPENDENCIES.md), [`docs/BUILD.md`](../BUILD.md).
- `CONTRIBUTING.md`, *Security*.
