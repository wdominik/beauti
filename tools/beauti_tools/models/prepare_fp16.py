# SPDX-License-Identifier: GPL-3.0-or-later
"""Developer-only FP16 conversion, retaining float32 model interfaces."""

import onnx
from onnxruntime.transformers.float16 import convert_float_to_float16

from ..paths import MODELS


def main():
    root = MODELS
    (root / 'fp16').mkdir(exist_ok=True)
    for name in ('skin_retouching.onnx',):
        model = convert_float_to_float16(onnx.load(root / 'optimized' / name), keep_io_types=True)
        # The converter may repeat identical casts of a shared Resize initializer.
        seen: dict[tuple[str, ...], tuple[object, ...]] = {}
        unique = []
        for node in model.graph.node:
            signature = (
                node.op_type,
                node.domain,
                tuple(node.input),
                tuple(a.SerializeToString() for a in node.attribute),
            )
            outputs = tuple(node.output)
            if outputs in seen:
                assert seen[outputs] == signature, 'Conflicting duplicate output'
            else:
                seen[outputs] = signature
                unique.append(node)
        del model.graph.node[:]
        model.graph.node.extend(unique)
        available = {v.name for v in model.graph.input} | {v.name for v in model.graph.initializer} | {''}
        pending = list(model.graph.node)
        ordered = []
        while pending:
            ready = [n for n in pending if all(i in available for i in n.input)]
            if not ready:
                raise RuntimeError('Invalid graph dependency')
            for node in ready:
                ordered.append(node)
                available.update(node.output)
                pending.remove(node)
        del model.graph.node[:]
        model.graph.node.extend(ordered)
        onnx.checker.check_model(model)
        onnx.save(model, root / 'fp16' / name)
        print(name, (root / 'fp16' / name).stat().st_size)


if __name__ == '__main__':
    main()
