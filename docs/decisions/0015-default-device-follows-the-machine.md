# 0015: The default device follows the machine: accelerator when present, otherwise CPU

Status: Accepted. Date: 2026-10-26.

## Context

Each build offers its accelerator first (Core ML on macOS, NVIDIA Full Precision
on Windows) and uses that first choice as the default for new filters
([0003](0003-nvidia-full-precision-default.md),
[0004](0004-apple-core-ml-default-and-cpu-skin-mask.md)). With one Windows
package for every machine, a user without
an NVIDIA GPU, or with a driver older than the pinned CUDA release, would see a
new filter fail on its first start and would have to find the Processing Device
list to make it work. [0007](0007-report-provider-failures.md) rightly forbids
replacing a chosen device silently; it says nothing about how the first choice
is made.

## Decision

- Before the first filter of a process is created, the build probes whether its
  accelerator can run on this machine, once, without loading the provider:
  Windows loads the NVIDIA driver (`nvcuda.dll`) and requires a driver of at
  least the pinned CUDA version (`buildspec.json`, `tensorrt-rtx`,
  `driver_api_minimum`), a GPU the provider supports (SM 8.6, 8.9, or 12.0
  and newer), and the four user-installed NVIDIA files in their pinned sizes; macOS requires the bundled runtime to register the Core ML
  provider.
- The default for new filters is the build's first choice when the probe
  succeeds and CPU otherwise (`choose_default` in `src/backends/backend.cpp`).
  The Processing Device list shows the chosen default; nothing is written to the
  user's settings.
- When the default fell back, the status line carries a suffix
  (`CPUFallbackSuffix`) while the filter runs on the CPU, and the OBS log names
  the reason once per process.
- A saved or explicitly chosen accelerator that fails is still reported, not
  replaced ([0007](0007-report-provider-failures.md)).

## Consequences

- One Windows package serves NVIDIA and non-NVIDIA machines; the CPU-only build
  remains a compile and test variant, not a release artifact.
- A machine that gains an NVIDIA GPU, a newer driver, or the NVIDIA files picks the accelerator up
  on the next OBS start for filters that never had a device saved; filters whose
  device was saved keep it.
- The probe costs one driver initialization on Windows when the first filter is
  created, not at OBS start.
- The driver minimum is a pin like the others: raising the CUDA release raises
  it in `buildspec.json`, and CMake passes it into the build.

## Evidence

- `src/backends/backend.cpp`, `probe_accelerator`, `choose_default`;
  `tests/native/provider_policy.cpp`.
- `src/plugin/filter.cpp`, `accelerator_missing` and the status suffix.
- The TensorRT RTX EP prerequisites (driver and GPU generations) and
  `buildspec.json`, `driver_requirement`.
