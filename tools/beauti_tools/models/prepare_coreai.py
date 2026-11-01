# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert the retouch model and the skin segmenter to Core AI assets for the macOS build.

Retouch: the decoder's bilinear x2 upsampling with align_corners=True is not a resize
the Neural Engine supports. It is replaced by the same interpolation as two
multiplications with fixed matrices, checked against the original (exact up to
rounding). The skin segmenter is converted from the ONNX graph that
prepare_mediapipe produced and runs on the GPU.

Debug locations are stripped so no local paths reach the bundle. The Core AI bytecode
writer does not order operations deterministically, so a new conversion has new file
hashes even from identical inputs; this step records them in models/manifest.json.
"""

import hashlib
import json
import shutil
import sys
from pathlib import Path
from typing import cast

import coreai_onnx
import torch
from coreai._compiler.passmanager import PassManager
from coreai_torch import TorchConverter, get_decomp_table

from ..paths import MODELS, ROOT

sys.path.insert(0, str(ROOT / 'third_party'))
from modelscope_unet.unet_deploy import UNet

OUTPUT = MODELS / 'coreai'
RETOUCH_SIDES = (384, 512)
# Largest difference the rewrites may introduce against the original graph.
REWRITE_TOLERANCE = 1e-4


def save_stripped(program, target: Path) -> None:
    # The converter exposes no public pass hook; strip-debuginfo runs on its MLIR module.
    module = program._module._mlir_module
    with module.context:
        PassManager.parse('builtin.module(strip-debuginfo)').run(module.operation)
    program.save_asset(target)


def interpolation_matrix(n: int) -> torch.Tensor:
    """Bilinear x2 with align_corners=True: output i samples input i * (n - 1) / (2n - 1)."""
    m = torch.zeros(2 * n, n, dtype=torch.float64)
    for i in range(2 * n):
        position = i * (n - 1) / (2 * n - 1)
        low = min(int(position), n - 1)
        high = min(low + 1, n - 1)
        m[i, low] += 1 - (position - low)
        m[i, high] += position - low
    return m


class MatrixUpsample(torch.nn.Module):
    def __init__(self, n: int, dtype: torch.dtype):
        super().__init__()
        m = interpolation_matrix(n).to(dtype)
        self.register_buffer('rows', m)
        self.register_buffer('columns', m.t().contiguous())

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        rows, columns = cast(torch.Tensor, self.rows), cast(torch.Tensor, self.columns)
        return torch.matmul(rows, torch.matmul(x, columns))


def load_unet() -> UNet:
    model = UNet(3, 3, init_weights=False).eval()
    checkpoint = torch.load(MODELS / 'skin_retouching.pt', map_location='cpu', weights_only=True)
    model.load_state_dict(checkpoint['generator'], strict=True)
    # Same Conv/BatchNorm fusion as the deployed ONNX graph.
    for module in model.modules():
        if isinstance(module, torch.nn.Sequential):
            children = list(module.children())
            for i in range(len(children) - 1):
                conv, norm = children[i], children[i + 1]
                if isinstance(conv, torch.nn.Conv2d) and isinstance(norm, torch.nn.BatchNorm2d):
                    module[i] = torch.nn.utils.fusion.fuse_conv_bn_eval(conv, norm)
                    module[i + 1] = torch.nn.Identity()
    return model


def matrix_unet(side: int, dtype: torch.dtype) -> UNet:
    model = load_unet()
    # The decoder inputs are side/16, side/8, side/4, and side/2.
    for level, block in enumerate([model.up1, model.up2, model.up3, model.up4]):
        block.up = MatrixUpsample(side // (16 >> level), dtype)
    return model.to(dtype)


def convert_retouch(side: int) -> None:
    x = torch.rand(1, 3, side, side, generator=torch.Generator().manual_seed(side)) * 2 - 1
    with torch.inference_mode():
        delta = float((matrix_unet(side, torch.float32)(x) - load_unet()(x)).abs().max())
    if delta > REWRITE_TOLERANCE:
        raise RuntimeError(f'Upsampling rewrite changed the retouch output by {delta}')
    model = matrix_unet(side, torch.float16)
    example = torch.zeros(1, 3, side, side, dtype=torch.float16)
    exported = torch.export.export(model, args=(example,)).run_decompositions(get_decomp_table())
    program = TorchConverter().add_exported_program(exported, input_names=['image'], output_names=['blend'])
    save_stripped(program.to_coreai(), OUTPUT / f'retouch-{side}.aimodel')
    print(f'retouch-{side}.aimodel', 'rewrite max difference', delta)


def convert_segmenter() -> None:
    save_stripped(
        coreai_onnx.convert(MODELS / 'native/skin_segmenter.onnx', repair=False), OUTPUT / 'skin-segmenter.aimodel'
    )
    print('skin-segmenter.aimodel')


def record_hashes() -> None:
    path = ROOT / 'models/manifest.json'
    manifest = json.loads(path.read_text())
    files = {k: v for k, v in manifest['files'].items() if not k.startswith('coreai/')}
    for stale in OUTPUT.glob('mask-*.aimodel'):
        shutil.rmtree(stale)
    for asset in sorted(OUTPUT.glob('*.aimodel/*')):
        files[asset.relative_to(MODELS).as_posix()] = hashlib.sha256(asset.read_bytes()).hexdigest()
    manifest['files'] = files
    path.write_text(json.dumps(manifest, indent=2) + '\n')


def main():
    OUTPUT.mkdir(exist_ok=True)
    torch.set_num_threads(4)
    for side in RETOUCH_SIDES:
        convert_retouch(side)
    convert_segmenter()
    record_hashes()


if __name__ == '__main__':
    main()
