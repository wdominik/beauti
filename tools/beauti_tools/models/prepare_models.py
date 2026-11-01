# SPDX-License-Identifier: GPL-3.0-or-later
"""Create the fixed-shape, single-output retouch ONNX graph; original weights stay unchanged."""

import hashlib
import json

import numpy as np
import onnx
import onnxruntime as ort

from ..paths import MODELS


def main():
    ROOT = MODELS
    reports = []
    for name, channels in [('skin_retouching.onnx', 3)]:
        source = ROOT / name
        dest = ROOT / 'optimized' / name
        dest.parent.mkdir(exist_ok=True)
        model = onnx.load(source)
        for dim, value in zip(model.graph.input[0].type.tensor_type.shape.dim, (1, 3, 512, 512)):
            dim.ClearField('dim_param')
            dim.dim_value = value
        first = model.graph.output[0]
        for dim, value in zip(first.type.tensor_type.shape.dim, (1, channels, 512, 512)):
            dim.ClearField('dim_param')
            dim.dim_value = value
        model = onnx.utils.Extractor(model).extract_model([model.graph.input[0].name], [first.name])
        del model.graph.value_info[:]
        model = onnx.shape_inference.infer_shapes(model, strict_mode=True)
        onnx.checker.check_model(model)
        temporary = dest.with_suffix('.input.onnx')
        onnx.save(model, temporary)
        options = ort.SessionOptions()
        options.intra_op_num_threads = 2
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
        options.optimized_model_filepath = str(dest)
        ort.InferenceSession(str(temporary), sess_options=options, providers=['CPUExecutionProvider'])
        result = onnx.load(dest)
        constants = {v.name: onnx.numpy_helper.to_array(v) for v in result.graph.initializer}
        aliases: dict[str, str] = {}
        nodes = []
        for node in result.graph.node:
            for i, value in enumerate(node.input):
                node.input[i] = aliases.get(value, value)
            if node.op_type == 'Pad' and node.input[1] in constants and np.all(constants[node.input[1]] == 0):
                aliases[node.output[0]] = node.input[0]
            else:
                nodes.append(node)
        del result.graph.node[:]
        result.graph.node.extend(nodes)
        result = onnx.utils.Extractor(result).extract_model([result.graph.input[0].name], [result.graph.output[0].name])
        onnx.checker.check_model(result)
        onnx.save(result, dest)
        assert all(node.domain in ('', 'ai.onnx') for node in result.graph.node)
        temporary.unlink()
        reports.append(
            {
                'model': name,
                'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                'optimized_sha256': hashlib.sha256(dest.read_bytes()).hexdigest(),
                'nodes': len(result.graph.node),
                'outputs': [o.name for o in result.graph.output],
                'bytes': dest.stat().st_size,
            }
        )
    print(json.dumps(reports, indent=2))


if __name__ == '__main__':
    main()
