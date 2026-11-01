# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert the MediaPipe face detector and skin segmenter from TFLite to ONNX.

Both graphs get planar NCHW input and the tensor names the engine uses. In the
segmenter, three nearest-neighbor Resize operations use TensorFlow's
tf_half_pixel_for_nn coordinates, which Core AI does not accept; for their integer
scale factors, asymmetric coordinates with floor rounding select the same pixels.
Each converted graph is compared with the TFLite model on random input.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import tensorflow as tf

from ..paths import MODELS

# Largest difference allowed against the TFLite model (float32 conversion rounding).
TOLERANCE = 1e-3
MODELS_TO_CONVERT = {
    'face_detector.onnx': ('blaze_face_full_range.tflite', 'image', ['boxes', 'scores']),
    'skin_segmenter.onnx': ('selfie_multiclass_256x256.tflite', 'image', ['classes']),
}


def rename(model: onnx.ModelProto, old: str, new: str) -> None:
    for node in model.graph.node:
        node.input[:] = [new if name == old else name for name in node.input]
        node.output[:] = [new if name == old else name for name in node.output]
    for value in [*model.graph.input, *model.graph.output]:
        if value.name == old:
            value.name = new


def standard_resize(model: onnx.ModelProto) -> int:
    changed = 0
    for node in model.graph.node:
        if node.op_type != 'Resize':
            continue
        attributes = {a.name: a for a in node.attribute}
        mode = attributes['coordinate_transformation_mode']
        if mode.s != b'tf_half_pixel_for_nn':
            continue
        if onnx.helper.get_attribute_value(attributes['mode']) != b'nearest':
            raise RuntimeError('Unexpected Resize mode in ' + node.name)
        mode.s = b'asymmetric'
        for attribute in [a for a in node.attribute if a.name == 'nearest_mode']:
            node.attribute.remove(attribute)
        node.attribute.append(onnx.helper.make_attribute('nearest_mode', 'floor'))
        changed += 1
    return changed


def max_difference(tflite: Path, onnx_path: Path, input_name: str) -> float:
    interpreter = tf.lite.Interpreter(model_path=str(tflite))
    interpreter.allocate_tensors()
    detail = interpreter.get_input_details()[0]
    x = np.random.default_rng(0).random(detail['shape'], dtype=np.float32) * 2 - 1
    interpreter.set_tensor(detail['index'], x)
    interpreter.invoke()
    reference = sorted(
        (interpreter.get_tensor(o['index']) for o in interpreter.get_output_details()), key=lambda a: a.size
    )
    session = ort.InferenceSession(str(onnx_path), providers=['CPUExecutionProvider'])
    actual = sorted(session.run(None, {input_name: x.transpose(0, 3, 1, 2).copy()}), key=lambda a: a.size)
    return max(float(np.abs(a.reshape(r.shape) - r).max()) for a, r in zip(actual, reference, strict=True))


def main():
    (MODELS / 'native').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        for target, (source, input_name, outputs) in MODELS_TO_CONVERT.items():
            converted = Path(scratch) / target
            original_input = tf.lite.Interpreter(model_path=str(MODELS / source)).get_input_details()[0]['name']
            subprocess.run(
                [
                    sys.executable,
                    '-m',
                    'tf2onnx.convert',
                    '--tflite',
                    str(MODELS / source),
                    '--output',
                    str(converted),
                    '--opset',
                    '17',
                    '--inputs-as-nchw',
                    original_input,
                ],
                check=True,
                capture_output=True,
            )
            model = onnx.load(converted)
            rename(model, model.graph.input[0].name, input_name)
            # Outputs ordered by size: detector boxes before scores is not guaranteed by the converter.
            ordered = sorted(model.graph.output, key=lambda o: -o.type.tensor_type.shape.dim[-1].dim_value)
            for value, name in zip(ordered, outputs, strict=True):
                rename(model, value.name, name)
            resized = standard_resize(model)
            onnx.checker.check_model(model)
            onnx.save(model, MODELS / 'native' / target)
            delta = max_difference(MODELS / source, MODELS / 'native' / target, input_name)
            if delta > TOLERANCE:
                raise RuntimeError(f'{target} differs from the TFLite model by {delta}')
            print(target, 'max difference', delta, 'resize nodes rewritten', resized)


if __name__ == '__main__':
    main()
