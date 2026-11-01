# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed 384 px retouch graphs for Performance mode; weights are unchanged."""

import onnx

from ..paths import MODELS


def main():
    root = MODELS
    (root / 'native').mkdir(exist_ok=True)
    # Performance mode: the same retouch weights at a fixed 384 px shape. This is an
    # explicit speed/quality mode, not an assertion of pixel equivalence.
    for variant in ('fp16', 'optimized'):
        m = onnx.load(root / variant / 'skin_retouching.onnx')
        for tensor in [*m.graph.input, *m.graph.output]:
            for i in (2, 3):
                tensor.type.tensor_type.shape.dim[i].dim_value = 384
        del m.graph.value_info[:]
        m = onnx.shape_inference.infer_shapes(m, strict_mode=True)
        onnx.checker.check_model(m)
        destination = root / 'native' / (variant + '-fast')
        destination.mkdir(exist_ok=True)
        onnx.save(m, destination / 'skin_retouching.onnx')


if __name__ == '__main__':
    main()
