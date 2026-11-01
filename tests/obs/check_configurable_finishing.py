# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent NumPy oracles for the configurable GPU texture and local chroma stages."""

import argparse
import json
from pathlib import Path

import cv2
import numpy as np

XYZ = np.array(
    [[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750], [0.0193339, 0.1191920, 0.9503041]]
)
INV = np.array(
    [[3.2404542, -1.5371385, -0.4985314], [-0.9692660, 1.8760108, 0.0415560], [0.0556434, -0.2040259, 1.0572252]]
)
WHITE = np.array([0.95047, 1, 1.08883])


def to_lab(rgb):
    c = rgb.astype(float) / 255
    linear = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    xyz = (linear @ XYZ.T) / WHITE
    f = np.where(xyz > 216 / 24389, np.cbrt(xyz), ((24389 / 27) * xyz + 16) / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], axis=-1)


def to_rgb(lab):
    fy = (lab[..., 0] + 16) / 116
    f = np.stack([fy + lab[..., 1] / 500, fy, fy - lab[..., 2] / 200], axis=-1)
    xyz = np.where(f > 6 / 29, f**3, (116 * f - 16) * 27 / 24389) * WHITE
    linear = np.clip(xyz @ INV.T, 0, 1)
    encoded = np.where(linear <= 0.0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - 0.055)
    return np.floor(encoded * 255 + 0.5).clip(0, 255).astype(np.uint8)


def sample(array, offset, axis):
    """Exact bilinear clamp samples for axis-aligned taps, without OpenCV's 1/32 grid."""
    coord = np.arange(array.shape[axis]) + offset
    low = np.floor(coord).astype(int)
    mix = coord - low
    shape = [1] * array.ndim
    shape[axis] = len(coord)
    mix = mix.reshape(shape)
    return (
        np.take(array, low.clip(0, len(coord) - 1), axis=axis) * (1 - mix)
        + np.take(array, (low + 1).clip(0, len(coord) - 1), axis=axis) * mix
    )


def color_reference(rgb, skin, strength, size, edges):
    lab = to_lab(rgb)
    guide, light = lab[..., 1:], lab[..., 0]
    spacing = np.clip(min(skin.shape) * 0.012 * size, 1, 32)
    field = guide.copy()
    for _ in range(2):
        for axis in (1, 0):
            total = np.zeros_like(field)
            weights = np.zeros_like(skin, dtype=float)
            for tap, kernel in zip(range(-2, 3), (1, 4, 6, 4, 1), strict=True):
                dl = (light - sample(light, tap * spacing, axis)) * edges / 8
                dc = np.linalg.norm(guide - sample(guide, tap * spacing, axis), axis=-1) * edges / 6
                weight = kernel * sample(skin, tap * spacing, axis) / (1 + dl**4 + dc**4)
                total += weight[..., None] * sample(field, tap * spacing, axis)
                weights += weight
            field = np.where((skin > 0)[..., None], total / np.maximum(weights[..., None], 1e-12), field)

    def ramp(a, b):
        t = np.clip((light - a) / (b - a), 0, 1)
        return t * t * (3 - 2 * t)

    delta = field - guide
    magnitude = np.linalg.norm(delta, axis=-1)
    weight = strength * skin * ramp(3, 15) * (1 - ramp(85, 98))
    delta *= (weight * 6 * np.tanh(magnitude / 6) / np.maximum(magnitude, 1e-6))[..., None]
    lab[..., 1:] += delta
    result = to_rgb(lab)
    result[skin == 0] = rgb[skin == 0]
    return result


def texture_reference(rgb, skin, band, size=1, edges=1):
    lab = to_lab(rgb)
    light = lab[..., 0].copy()
    level, delta = light.copy(), np.zeros_like(light)
    spacing = np.clip(np.floor(min(skin.shape) / 700 + 0.5), 1, 6)
    for scale, threshold in enumerate((6, 7, 9, 12)):
        previous = level
        for axis in (1, 0):
            total, weights = np.zeros_like(light), np.zeros_like(light)
            n = skin.shape[axis]
            for tap, kernel in zip(range(-2, 3), (1, 4, 6, 4, 1), strict=True):
                index = (np.arange(n) + int(tap * spacing * 2**scale * size)) % (2 * (n - 1))
                index = np.minimum(index, 2 * (n - 1) - index)
                d = (light - np.take(light, index, axis=axis)) / threshold * edges
                weight = kernel * np.take(skin, index, axis=axis) / (1 + d**4)
                total += weight * np.take(level, index, axis=axis)
                weights += weight
            level = np.where(skin > 0, total / np.maximum(weights, 1e-12), level)
        if scale == band:
            delta += level - previous
    lab[..., 0] += skin * 6 * np.tanh(delta / 6)
    result = to_rgb(lab)
    result[skin == 0] = rgb[skin == 0]
    return result


def run(directory):
    p = Path(directory)
    meta = json.loads((p / 'maps.json').read_text())
    w, h, n = meta['width'], meta['height'], meta['mask_size']
    l, t, r, b = meta['box']
    rw, rh = r - l, b - t

    def raw(name):
        return np.fromfile(p / (name + '.rgba'), np.uint8).reshape(h, w, 4)

    original = raw('chart-original')
    crop = original[t:b, l:r, :3]
    mask = np.fromfile(p / 'chart-mask.f32', np.float32).reshape(n, n, 4)
    skin = cv2.resize(mask[..., 0], (rw, rh), interpolation=cv2.INTER_LINEAR) * (
        cv2.resize(mask[..., 1], (rw, rh), interpolation=cv2.INTER_NEAREST) > 0.5
    )
    y, x = np.mgrid[:rh, :rw]
    edge = np.minimum(np.minimum(x, rw - 1 - x) / max(1, rw * 0.04), np.minimum(y, rh - 1 - y) / max(1, rh * 0.04))
    skin = skin * np.clip(edge, 0, 1)
    outside = np.ones((h, w), bool)
    outside[t:b, l:r] = False
    records = []
    for kind, cases in [('color', 4), ('texture', 5)]:
        for i in range(cases):
            name = f'chart-{kind}-{i}'
            actual = raw(name)
            expected = (
                color_reference(crop, skin, 0.25 if i == 0 else 1, 2 if i == 2 else 1, 0 if i == 3 else 1)
                if kind == 'color'
                else texture_reference(crop, skin, i % 4, 2 if i == 4 else 1, 0.5 if i == 4 else 1)
            )
            np.testing.assert_array_equal(actual[outside], original[outside])
            np.testing.assert_array_equal(actual[..., 3], original[..., 3])
            np.testing.assert_array_equal(actual[t:b, l:r, :3][skin == 0], crop[skin == 0])
            error = np.abs(actual[t:b, l:r, :3].astype(float) - expected)
            record = {
                'case': name,
                'mean_error': float(error.mean()),
                'p99_error': float(np.quantile(error, 0.99)),
                'max_error': int(error.max()),
            }
            assert record['mean_error'] < 0.15 and record['p99_error'] <= 1 and record['max_error'] <= 2, record
            assert np.any(actual != original), name
            if kind == 'color':
                light_error = np.abs(to_lab(actual[t:b, l:r, :3])[..., 0] - to_lab(crop)[..., 0])
                record['max_lightness_change_after_rgb8'] = float(light_error.max())
                assert light_error.max() < 0.5, record
            records.append(record)
    alpha = raw('chart-alpha-original')
    result = raw('chart-alpha')
    np.testing.assert_array_equal(alpha[..., 3], result[..., 3])
    np.testing.assert_array_equal(alpha[outside], result[outside])
    report = {'passed': True, 'cases': records, 'alpha_and_exclusions_exact': True}
    (p / 'configurable-finishing.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    run(parser.parse_args().directory)


if __name__ == '__main__':
    main()
