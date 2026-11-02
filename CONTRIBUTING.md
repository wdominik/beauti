# Contributing to Beauti

This page is the binding set of conventions for the repository. It follows the
C++ Core Guidelines, the OBS Studio plugin conventions, PEP 8/621/735 for Python,
and target-based CMake. Where Beauti deviates, the reason is stated, and the
decision records under `docs/decisions/` hold the reasoning.

## Language

Everything in the repository is written in US English: identifiers, code
comments, log and error messages, docstrings, CMake messages, shader comments,
commit messages, the `en-US` locale catalog, the documentation under `docs/`,
and the decision records. Use US spellings (*color*, *behavior*, *analyze*,
*optimize*, *initialize*, *license*, *catalog*, *gray*, *canceled*); the
pre-commit hook `us-english` rejects the common British forms. Plural nouns such
as *analyses* are not affected. The only files in another language are the
locale catalogs other than `en-US.ini` and the copied upstream files under
`third_party/` and `licenses/`.

## Development setup

```sh
uv sync --group dev                 # Python 3.14, tools package, CMake, Ninja, formatters
uv run pre-commit install
uv run cmake --workflow --preset macos   # Windows: see docs/BUILD.md
```

`pyproject.toml` declares the tools package `beauti_tools` (entry points
`beauti-build`, `beauti-package`, ...) and the PEP 735 dependency groups `dev`,
`test`, and `models`; `uv.lock` pins every package for all platforms. The
pre-commit hooks run in their own environments with the same pinned versions, so
they behave identically on macOS, Linux, and Windows. Generated dependencies live
in `.cache/`, build output in `build/`, packages in `dist/`; all three are
ignored by Git and excluded from source exports. Clone outside iCloud-synced
folders; see the note in [docs/BUILD.md](docs/BUILD.md).

## Repository layout

| Directory | Contents |
| --- | --- |
| `src/plugin/` | OBS filter lifecycle, settings, GPU composition (`filter.cpp`, `gpu.cpp`) |
| `src/core/` | Native inference engine, paths, timing, and render-thread policies without OBS (continuity, face tracking, capture pacing, strength ramp, scene cut, live statistics) |
| `src/backends/` | Device selection and model plans; the Core AI adapter (Swift, C interface); TensorRT RTX and CPU providers |
| `src/effects/` | Portable C++ reference finishing algorithms (separate shared library) |
| `data/` | OBS effect shaders and locale catalogs |
| `cmake/` | Dependency fetching (`Dependencies.cmake`), OBS imports (`OBS.cmake`), templates |
| `tools/beauti_tools/` | Python package: `build` (staging, Windows SDK), `models` (export recipes), `packaging` (audits, export), `bench` (worker protocol), verification |
| `tools/benchmarks/` | Headless engine worker (C++) |
| `models/` | Model identities and acquisition records |
| `tests/` | Native tests (Catch2 via CTest), pytest suites, libobs fixture runners, numerical references, build-pipeline tests |
| `third_party/` | Copied build headers and model-export source; byte-identical to upstream |
| `docs/` | Reference documentation; `docs/decisions/` holds the decision records |
| `licenses/` | Third-party license texts and notices |
| `.github/` | CI workflow and Dependabot configuration |

`buildspec.json` pins every build-time dependency (URL, SHA-256, archive root).
`VERSION` is the single product version source. `CHANGELOG.md` records every
user-visible change under *Unreleased*.

## C++

**Standard.** C++20 without compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`), on
Apple Clang and MSVC. C++23 is adopted once MSVC ships `/std:c++23` as its default
and Xcode supports the features we would use; until then no C++23 library features.
Objective-C++ test sources follow the same standard.

**Formatting.** `.clang-format` is the OBS Studio style: tabs, 8-wide indentation,
120 columns, Linux braces, pointers right-aligned. Run `clang-format -i` before
committing; CI rejects unformatted files. `third_party/` is never reformatted.

**Warnings.** Code compiles without warnings under the project flags
(`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wold-style-cast` on Clang/GCC,
`/W4` on MSVC). Third-party headers are SYSTEM includes; do not silence warnings in
project code with pragmas. `-Wsign-conversion` is the one class still disabled;
new code should already be clean under it (use `size_t` for indices and sizes).

**Naming.** Types `PascalCase` (`SkinGPU`, `RenderContext`); functions, variables,
members, and namespaces `snake_case`; `constexpr` constants `kCamelCase`
(`kStrengthRampNs`); macros `UPPER_CASE` with the `BEAUTI_` prefix; enumerators
`PascalCase` inside `enum class`. File names `snake_case.cpp/.h`.

**Headers and includes.** `#pragma once`; one SPDX line at the top of every source
file. Include order: the file's own header, project headers, OBS/ONNX headers,
standard library. No `using namespace` at file scope. Keep headers minimal: forward
declare where a full type is not needed.

**Ownership and resources.** Every resource has exactly one RAII owner. Types that
own handles delete copy construction and assignment (`Session`, `Engine`,
`SkinGPU`). Prefer `std::unique_ptr` with a deleter type over raw pointers plus a
manual `destroy`; the OBS graphics handles use the deleter types in
`src/plugin/gs_handles.h`. No `new`/`delete` in new code.

**Errors.** Exceptions are the error mechanism inside C++. They never cross the OBS
C callbacks or the `extern "C"` effects API: catch at the boundary, log with
`blog(LOG_ERROR, "[beauti] ...")`, and return the C contract's failure value.
Validate external input (settings, protocol headers, files) before use and fail
with a message that names what was expected.

**Threads.** Two threads touch the filter: the OBS graphics thread and the worker.
Document for every `Filter` member which thread owns it or which mutex guards it.
The graphics thread never blocks on inference; the worker never touches GPU
objects. New shared state goes through the existing mutex or an atomic with a
comment explaining the ordering. The worker never calls `obs_module_text` or other
functions that read module state: libobs defers source destruction and can free the
module's locale table before the worker is joined, so the strings it needs are
copied when the filter is created.

**Constants and configuration.** No magic numbers: a named `constexpr` with a
one-line reason. Values shared between the shader path and the CPU reference live in
one header. The settings keys in the table in
`docs/ARCHITECTURE.md` and the preset IDs in `src/core/presets.h` become a
compatibility contract with 1.0 (see *Versioning and releases*).

**Enums and casts.** `enum class` for new enumerations. `static_cast`,
`reinterpret_cast`, or functional casts (`float(x)`) only; no C-style casts.

**Comments.** Explain why, not what. Every source file starts with one to three
lines saying what it holds and where it sits in the pipeline. Every function or
block that implements an algorithm, a protocol, a threading rule, a workaround, or
a measured decision carries a one- or two-line comment stating the idea (what the
numbers mean, which convention is matched, where the measurement lives); the code
then speaks for itself. No comment restates a statement, and no line-by-line
narration. The same rule applies to shaders, CMake, and Python.

**Size.** Functions stay under about 50 lines and a cognitive complexity of 25
(clang-tidy `readability-function-size`, `readability-function-cognitive-complexity`).
Split by responsibility, as `Filter::render()` demonstrates.

## Shaders

`data/beauti.effect` is OBS effect HLSL. Every technique has a counterpart in the
CPU references under `src/effects/` or a documented reason why not. Pass slots are
named in `SkinGPU::Pass`; the pass graph is drawn in `docs/ARCHITECTURE.md` and must
be updated with any new pass. Shader changes are verified with `beauti-gpu-check`
and `tests/obs/check_finishing_pixels.py` before and after.

## Swift

Swift appears in one file, `src/backends/coreai.swift`, because Core AI has no C or
Objective-C interface ([decision 0018](docs/decisions/0018-core-ai-replaces-core-ml.md)).
It stays a thin adapter: load, run, release behind the C functions in
`src/backends/coreai.h`; all logic, validation, and threading rules live in C++.
Errors are returned as messages and become exceptions in `CoreAIModel`. Formatting
follows the C++ files (tabs, 120 columns); the build compiles it without warnings.

## Python

**Role.** Python is a development tool for model preparation, Windows SDK
preparation, packaging, verification, and tests. The plugin ships no Python. The
tools form the package `beauti_tools` under `tools/` with one console entry point
per task (`[project.scripts]` in `pyproject.toml`); modules that run right after a
build (`verify_native`, `bench.protocol`) use the standard library only.

**Version and style.** Python 3.14 is the pinned interpreter line (`.python-version`;
`requires-python = ">=3.14"`). `ruff format`
and `ruff check` with the rule set in `pyproject.toml` (pycodestyle, pyflakes,
isort, pyupgrade, bugbear, simplify, perf, pathlib, pylint subsets, refurb, ruff,
quotes) are the only style authority; single quotes, 120 columns. Exceptions are
declared in `pyproject.toml`, never with inline `noqa` unless unavoidable.

**Structure.** New modules get type annotations and a module docstring that states
what the module verifies or produces. `mypy` runs over the package in CI and the
pre-commit hook; `build`, `bench`, `paths`, and `source_files` are fully annotated
and stay that way (`disallow_untyped_defs`), the rest is checked with
`check_untyped_defs` until it is annotated. Every command has a `main()` registered in
`[project.scripts]`, uses `argparse`, `pathlib`, relative imports inside the package,
and explicit `subprocess.run(..., check=...)`. No `sys.path` manipulation; tests
import `beauti_tools` from the installed environment.

**Dependencies.** Declared in `pyproject.toml`: the package's own runtime needs
under `[project]`, everything else in `[dependency-groups]` (`dev`, `test`,
`models`), all pinned to exact versions and locked in `uv.lock` (`uv lock` after a
change, `uv sync --locked` to install). Verify output hashes after changing a
model-tooling version; serialization can change.

## CMake

Target-based: compile options, definitions, and include directories attach to
targets (`beauti-options` carries the shared flags). The only global settings are
the language standards and the sanitizer flags (`BEAUTI_SANITIZER`, which must also
instrument the test framework). Options and cache variables carry the `BEAUTI_` prefix.
Third-party headers are SYSTEM includes. Dependencies are declared in
`buildspec.json` and fetched by `cmake/Dependencies.cmake` with SHA-256
verification; no unpinned downloads anywhere. Presets are the user interface:
`cmake --workflow --preset <name>` must work on a clean checkout; the `*-ci` presets
add `CMAKE_COMPILE_WARNING_AS_ERROR` and are what CI runs, so a warning fails the
build. Hardening flags (`-fstack-protector-strong`, `_FORTIFY_SOURCE=2`; `/guard:cf`,
`/sdl` on MSVC) are part of `beauti-options`. `cmake_minimum_required` states a
policy range (`3.25...4.4`). In-source builds are rejected.

## Shell scripts

None. The repository contains no shell or PowerShell code; installation is a
documented copy, and packages are produced by the Python tools until CPack
replaces that step. Do not add scripts for what CMake, the tools package, or
the documentation can do.

## Testing

**Native.** Native tests are Catch2 (v3, pinned in `buildspec.json`) test cases in
`tests/native/`, compiled into `beauti-tests` and registered with CTest by tag. One
`TEST_CASE` per contract, named as the sentence it asserts; `GENERATE` for parameter
sets, `CAPTURE` for the values a failure report needs. No hand-written `require()`
helpers or `main()` functions.

**Python.** pytest is the only runner (`uv run pytest`); its configuration is in
`pyproject.toml`, the shared fixtures and the environment variables they read in
`tests/conftest.py`. Integration checks are `run(...)` functions in `check_*.py`
modules that also keep an `argparse` `main()` for manual and Windows runs; the
`test_*.py` modules call them and carry the markers `needs_reference`,
`needs_bundle`, and `needs_gpu`. A test whose prerequisite is missing skips and names
the variable to set; it never fails for that reason. New tests are pytest functions;
the `unittest`-style reference tests stay as they are.

**Sanitizers.** `cmake --workflow --preset macos-asan` and `macos-tsan` instrument
every target. A change to the worker, the render path, the engine's threading, or
the continuity policy runs the fault-injection suite and the slider checks under
ThreadSanitizer before it is committed. Sanitized builds are never packaged.

Before a pull request: `uv run cmake --workflow --preset macos` (configure, build,
CTest), `uv run pytest`, `uv run ruff format --check . && uv run ruff check . && uv run mypy`,
`uv run clang-format --dry-run -Werror` on changed C++ files, and
`uv run beauti-verify-provenance`. Changes to the engine or the
compositor are verified against the previous commit with the A/B method in
`docs/TESTING.md`: `beauti-gpu-check` outputs, `compare_workers.py --exact`, and the
libobs fixture runner. Changes to presentation or continuity run the fault-injection
suite. Record what you ran and the key results in the commit message or the pull
request; raw measurement output is not committed.

## Documentation

US English. `README.md` is for users; `docs/` is reference and describes the
current state; history lives in `CHANGELOG.md` and Git. Diagrams are Mermaid in
Markdown. A decision that shapes the product (a default, a provider policy, a
threading rule, a minimum host version, a tool or language choice) gets a record
in `docs/decisions/` with context, decision, consequences, and the measurements it
rests on; a reversal is a new record that supersedes the old one. A new or
renamed settings key is added to the settings table in `docs/ARCHITECTURE.md`. Every user-visible change adds a line under *Unreleased* in
`CHANGELOG.md`. UI text lives in `data/locale/en-US.ini`; every new key goes into
every catalog in `data/locale/` in the same commit (`tests/locale/check_locales.py`
runs as a pre-commit hook and a test; OBS would otherwise fall back to English
for that one string). Translations follow the terms OBS itself uses in that
language. Locale strings are words, never format strings; text with numbers is
composed in code with `std::format`. Refresh the provenance snapshot (`uv run beauti-update-snapshot`)
in the same commit as a source change; `beauti-verify-provenance` runs in CI.

## License and contributions

Beauti is GPL-3.0-or-later; the copyright holder is wdominik. Every source file
starts with `SPDX-License-Identifier: GPL-3.0-or-later` in its comment syntax (C++,
Swift, shaders, CMake, Python, templates); data files such as JSON and locale
catalogs carry none. Copied upstream files keep their own license and notices.

The copyright holder also uses Beauti's code under other terms. A contribution
can therefore be merged only if its author agrees, in the pull request, to the
following grant:

> I license my contribution under GPL-3.0-or-later with the additional permission in
> `licenses/GPL-Additional-Permission.txt`. In addition, I grant wdominik a
> perpetual, worldwide, non-exclusive, royalty-free, irrevocable license to use,
> modify, sublicense, and distribute my contribution under any other terms. I
> confirm that I have the right to grant these licenses and that the contribution
> contains no third-party material other than what I identify.

Contributions without this statement are not merged.

## Git

Branch from `main`; one logical change per commit. Subject line in the imperative,
at most 72 characters, no trailing period; the body says why, what was verified,
and what was deliberately left out. Formatting-only commits are separate from
behavior changes. Never commit generated files, build output, or model weights.
Dependabot pull requests are reviewed for changelog entries before merging; action
updates must keep the SHA pin with the version comment. Native pins follow the
procedure in [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md), which also lists every
dependency with its scope.

## Versioning and releases

Semantic Versioning. `VERSION` is bumped in the release commit together with the
`CHANGELOG.md` section. Before 1.0, saved settings are not migrated: settings that
no longer fit are reset to the defaults (processing settings are kept where their
keys still exist), and the code contains no version-specific mapping. From 1.0,
renaming or reinterpreting a saved setting needs an explicit migration and a
major version. Tags are `vX.Y.Z`. The gates and steps are in
[docs/RELEASE.md](docs/RELEASE.md). Record imported source and model origins before
including them: update `models/sources.json` for acquired bytes,
`models/manifest.json` for prepared runtime identities, and `docs/provenance.json`
through the snapshot tool. Hash updates do not replace review of changed source,
model transformations, or license conditions.

## Security

No network access at runtime; all build-time downloads are hash-pinned; archives
are extracted with path checks; Windows DLLs are loaded from the plugin directory
with restricted search flags. Do not add telemetry. Report a suspected
vulnerability privately to the maintainer before opening an issue.
