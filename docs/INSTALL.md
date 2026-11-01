# Installation

Beauti needs OBS Studio 33.0.0 or later. Quit OBS before you install or update.

## macOS

1. Download `Beauti-Mac-<version>-arm64.zip` from the
   [GitHub releases](https://github.com/wdominik/beauti/releases) and extract it.
2. Copy `beauti.plugin` into `~/Library/Application Support/obs-studio/plugins/`.
   Create the folder if it does not exist.
3. This build is not notarized, so macOS blocks it until you clear the download flag.
   Run this in Terminal:
   ```sh
   xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/beauti.plugin
   ```
4. Start OBS.

Beauti uses Apple Acceleration automatically. To update, replace `beauti.plugin`.

## Windows

You need the current x64
[Microsoft Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).

### 1. Install Beauti

1. Download `Beauti-Windows-<version>-x64.zip` from the
   [GitHub releases](https://github.com/wdominik/beauti/releases) and extract it.
2. Copy the `beauti` folder into `%ProgramData%\obs-studio\plugins\`. Windows may ask
   for administrator permission.
3. Start OBS and add the filter.

Beauti now runs on the CPU, which is considerably slower. On a machine with a
supported NVIDIA GPU, continue with the next step.

### 2. Add NVIDIA acceleration

Beauti does not include NVIDIA's runtime; you download it from NVIDIA. You need an
NVIDIA RTX GPU (Ampere or newer, such as RTX 30 series) and driver 580.88 or newer.

1. Quit OBS.
2. From [NVIDIA's release page](https://github.com/NVIDIA/TensorRT-RTX-EP-ABI/releases/tag/v0.5.0),
   download exactly this file: `TensorRT-RTX-EP-ABI-v0.5.0-cu13.zip`. Other versions
   do not work with this Beauti version. NVIDIA's
   [license agreement](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/reference/sla.html)
   applies to the download and its use.
3. Extract the archive and copy these four files into
   `%ProgramData%\obs-studio\plugins\beauti\`, next to `beauti.dll`:
   - `onnxruntime_providers_nv_tensorrt_rtx.dll`
   - `tensorrt_rtx_1_7.dll`
   - `tensorrt_onnxparser_rtx_1_7.dll`
   - `tensorrt_plugins.dll`
4. Start OBS. New filters now use NVIDIA Acceleration. For an existing filter, select
   it under **Processing Device**.

The first start takes longer because the models are compiled for your GPU once.

### Updating

Quit OBS and copy the new `beauti` folder over the existing one, without deleting it
first, so the NVIDIA files stay in place. If the new version's notes name another
NVIDIA version, replace the four files as well.

### Troubleshooting

- **The status line says the NVIDIA files are missing:** check that all four files
  are directly next to `beauti.dll`, then restart OBS.
- **The filter reports a wrong NVIDIA file version:** download the exact archive
  named above and copy the four files again.
- **The status line says no acceleration was found:** check your GPU and driver.

## Uninstalling

Quit OBS and delete `beauti.plugin` or the `beauti` folder.
