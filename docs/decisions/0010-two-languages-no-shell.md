# 0010: Two languages: C++ for the product, Python for the tooling, no shell scripts

Status: Accepted; amended by [0018](0018-core-ai-replaces-core-ml.md) for a Swift adapter to Core AI. Date: 2026-10-25.

## Context

Four languages were in use: C++ for the plugin, Python scripts with ad hoc
`sys.path` manipulation for build and verification, a zsh installer for macOS,
and PowerShell for the Windows installer and one test. Every language adds a
toolchain, a formatter, a style, and a review burden; the installers also
duplicated what a copy into the OBS plugin directory achieves.

## Decision

- C++20 for everything that ships: plugin, engine, reference library, test
  worker. CMake with presets is the build interface; no Python is needed to
  configure, build, or test on macOS.
- Python 3.14 for development tooling only, as the installable package
  `beauti_tools` with one console command per task, declared in `pyproject.toml`
  and locked with `uv.lock`. Model export recipes are part of the tooling.
- No shell, zsh, or PowerShell code in the repository. Installation is a
  documented copy; CI steps are single commands.

## Consequences

- There are no installer scripts; `README.md` and the package README describe
  the copy, update, and uninstall steps.
- Platform installers (`.pkg`, signed Windows installer) are deferred to a
  release-workflow decision; when they come, CPack generates them rather than
  hand-written scripts.
- Windows SDK preparation (import libraries from the official build) stays in
  Python because it needs `pefile`; it runs once per dependency update.

## Evidence

- `CONTRIBUTING.md`, *Python* and *Documentation*; `CHANGELOG.md`, *Removed*.
