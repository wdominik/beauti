# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare OBS eye output with an independent NumPy linear-light reference."""

import argparse
import json
from pathlib import Path

import cv2
import numpy as np


def ramp(low, high, value):
    t = np.clip((value - low) / (high - low), 0, 1)
    return t * t * (3 - 2 * t)


def enhance(rgb, masks, amount):
    """Reference for the documented exposure/chroma formula, independent of GPU code."""
    c = rgb / 255.0
    linear = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    y = (linear @ np.array([0.2126, 0.7152, 0.0722]))[..., None]
    protect = ramp(0.008, 0.045, y) * (1 - ramp(0.55, 0.9, y))
    chroma = np.ptp(c, axis=2, keepdims=True)
    iris = masks[..., :1] * amount * protect
    white = masks[..., 1:2] * amount * protect * ramp(0.06, 0.2, y) * (1 - ramp(0.12, 0.32, chroma))
    lifted = (linear + (linear - y) * 0.04 * iris) * np.exp2(0.45 * iris + 0.16 * white)
    delta = lifted - linear
    scale = np.minimum(1, ((1 - linear) / np.maximum(delta, 0.000001)).min(axis=2, keepdims=True))
    result = np.clip(linear + delta * scale, 0, 1)
    encoded = np.where(result <= 0.0031308, result * 12.92, 1.055 * result ** (1 / 2.4) - 0.055)
    return np.rint(encoded * 255).clip(0, 255).astype(np.uint8)


def run(directory):
    p = Path(directory)
    meta = json.loads((p / 'maps.json').read_text())
    w, h, n = meta['width'], meta['height'], meta['eye_size']
    l, t, r, b = meta['box']
    original = np.fromfile(p / 'original.rgba', np.uint8).reshape(h, w, 4)
    masks = np.zeros((h, w, 2), np.float32)
    if n:
        eye = np.fromfile(p / 'eyes.f32', np.float32).reshape(n, n, 4)
        masks[t:b, l:r] = cv2.resize(eye[..., :2], (r - l, b - t), interpolation=cv2.INTER_LINEAR)
    protected = masks.max(axis=2) == 0
    records = []
    for amount in (25, 50, 100):
        actual = np.fromfile(p / f'eyes-{amount}.rgba', np.uint8).reshape(h, w, 4)
        expected = enhance(original[..., :3], masks, amount / 100)
        error = np.abs(actual[..., :3].astype(int) - expected.astype(int))
        assert error.max() <= 1, error.max()
        assert np.array_equal(actual[..., 3], original[..., 3])
        assert np.array_equal(actual[protected], original[protected])
        records.append({'strength': amount, 'max_error': int(error.max()), 'mean_error': float(error.mean())})
    alpha_checked = (p / 'alpha-eyes.rgba').exists()
    if alpha_checked:
        source = np.fromfile(p / 'alpha-original.rgba', np.uint8).reshape(h, w, 4)
        result = np.fromfile(p / 'alpha-eyes.rgba', np.uint8).reshape(h, w, 4)
        alpha = source[..., 3:4].astype(float) / 255
        straight = np.divide(source[..., :3], alpha, out=np.zeros((h, w, 3)), where=alpha > 0)
        expected = np.rint(enhance(straight, masks, 1) * alpha).clip(0, 255).astype(np.uint8)
        assert np.abs(result[..., :3].astype(int) - expected.astype(int)).max() <= 1
        assert np.array_equal(result[..., 3], source[..., 3])
        assert np.array_equal(result[protected], source[protected])
        assert np.array_equal(result[source[..., 3] == 0], source[source[..., 3] == 0])
    report = {'passed': True, 'protected_pixels_exact': True, 'alpha_checked': alpha_checked, 'cases': records}
    (p / 'eye-pixels.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    run(parser.parse_args().directory)


if __name__ == '__main__':
    main()
