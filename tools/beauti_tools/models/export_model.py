# SPDX-License-Identifier: GPL-3.0-or-later
"""One-time developer tool. Runtime uses ONNX only, without PyTorch."""

import hashlib
import json
import sys

import numpy as np
import onnx
import onnxruntime as ort
import torch

from ..paths import MODELS, ROOT

sys.path.insert(0, str(ROOT / 'third_party'))
from modelscope_unet.unet_deploy import UNet


def main():
    torch.set_num_threads(4)
    model = UNet(3, 3, init_weights=False).eval()
    checkpoint = torch.load(MODELS / 'skin_retouching.pt', map_location='cpu', weights_only=True)
    model.load_state_dict(checkpoint['generator'], strict=True)
    torch.manual_seed(42)
    x = torch.rand(1, 3, 512, 512) * 2 - 1
    target = MODELS / 'skin_retouching.onnx'
    with torch.inference_mode():
        expected = model(x).numpy()
        torch.onnx.export(
            model, (x,), str(target), input_names=['image'], output_names=['blend'], opset_version=17, dynamo=False
        )
    onnx.checker.check_model(str(target))
    session = ort.InferenceSession(str(target), providers=['CPUExecutionProvider'])
    actual = session.run(None, {'image': x.numpy()})[0]
    delta = float(np.max(np.abs(expected - actual)))
    assert delta < 1e-4, delta
    report = {
        'max_abs_error_vs_pytorch': delta,
        'bytes': target.stat().st_size,
        'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
        'input': [1, 3, 512, 512],
        'opset': 17,
    }
    (MODELS / 'model-export-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
