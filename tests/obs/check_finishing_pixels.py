# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare real OBS GPU output with the CPU C++ reference; no camera required."""

import argparse
import json
import sys
from pathlib import Path

import cv2
import numpy as np

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tests/reference'))
from beauti_reference.dodge_burn import micro_dodge_burn
from beauti_reference.skin_tone import equalize_skin_tone, mix_linear
from beauti_reference.texture import reduce_skin_texture


def mix(a, b, amount):
    return np.rint(a.astype(np.float32) * (1 - amount) + b.astype(np.float32) * amount).astype(np.uint8)


def run(directory):
    """Check a beauti-gpu-check output directory; writes and returns its report."""
    p = Path(directory)
    meta = json.loads((p / 'maps.json').read_text())
    w, h = meta['width'], meta['height']
    l, t, r, b = meta['box']
    rw, rh = r - l, b - t

    def raw(name):
        return np.fromfile(p / (name + '.rgba'), np.uint8).reshape(h, w, 4)

    original = raw('original')
    m = np.fromfile(p / 'mask.f32', np.float32).reshape(meta['mask_size'], meta['mask_size'], 4)
    if meta.get('raw_mask', False):
        # Independent CPU reference for the shared GPU mask preparation.
        hard = m[..., 0].copy()
        confidence = m[..., 1].copy()
        inset = cv2.erode(
            (hard > 0.5).astype(np.float32),
            cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5)),
            borderType=cv2.BORDER_CONSTANT,
            borderValue=1,
        )
        m[..., 0] = (
            cv2.GaussianBlur(inset, (17, 17), 2, borderType=cv2.BORDER_REFLECT_101)
            * (hard > 0.5)
            * np.clip((confidence - 0.45) / 0.45, 0, 1)
        )
        m[..., 1] = hard
    skin = cv2.resize(m[..., 0], (rw, rh), interpolation=cv2.INTER_LINEAR) * (
        cv2.resize(m[..., 1], (rw, rh), interpolation=cv2.INTER_NEAREST) > 0.5
    )
    y, x = np.mgrid[:rh, :rw]
    edge = np.clip(
        np.minimum(np.minimum(x, rw - 1 - x) / max(1, rw * 0.04), np.minimum(y, rh - 1 - y) / max(1, rh * 0.04)),
        0,
        1,
    )
    skin = (skin * edge).astype(np.float32)
    protected = skin == 0
    outside = np.ones((h, w), bool)
    outside[t:b, l:r] = False
    active = skin > 0.65
    np.testing.assert_array_equal(raw('r0-t0-d0-s0'), original)
    np.testing.assert_array_equal(raw('empty-mask'), original)
    checks = []
    for retouch in [0, 60, 100]:
        base = raw(f'r{retouch}-t0-d0-s0')[t:b, l:r, :3]
        endpoint, _ = equalize_skin_tone(base, skin)
        for tone in [0, 50, 100]:
            toned = mix_linear(base, endpoint, tone / 100)
            for dodge in [0, 50, 100]:
                balanced, _ = micro_dodge_burn(toned, skin, dodge / 100)
                smoothed, _ = reduce_skin_texture(balanced, skin)
                for smooth in [0, 50, 100]:
                    name = f'r{retouch}-t{tone}-d{dodge}-s{smooth}'
                    actual = raw(name)
                    crop = actual[t:b, l:r, :3]
                    np.testing.assert_array_equal(actual[outside], original[outside])
                    np.testing.assert_array_equal(actual[..., 3], original[..., 3])
                    np.testing.assert_array_equal(crop[protected], base[protected])
                    reference = mix(balanced, smoothed, smooth / 100)
                    error = np.abs(crop.astype(float) - reference.astype(float))
                    record = {
                        'retouch': retouch,
                        'skin_tone': tone,
                        'dodge_burn': dodge,
                        'smooth': smooth,
                        'mean_rgb_error': float(error[active].mean()),
                        'p99_rgb_error': float(np.quantile(error[active], 0.99)),
                        'max_rgb_error': int(error.max()),
                    }
                    assert record['mean_rgb_error'] < 1.0 and record['p99_rgb_error'] <= 3, record
                    checks.append(record)
    combined = raw('combined-sequence')
    explicit = raw('explicit-sequence')
    sequence_error = np.abs(combined.astype(float) - explicit.astype(float))
    assert sequence_error.max() <= 1, (sequence_error.max(), sequence_error.mean())
    alpha = raw('alpha-original')
    alpha_out = raw('alpha-result')
    np.testing.assert_array_equal(alpha[..., 3], alpha_out[..., 3])
    assert np.all(alpha_out[alpha[..., 3] == 0] == 0)
    np.testing.assert_array_equal(alpha_out[outside], alpha[outside])
    assert np.isfinite(raw('high-quality')).all()
    report = {
        'passed': True,
        'combinations': 81,
        'box': meta['box'],
        'cpu_reference': checks,
        'explicit_sequence_max_rgb_error': int(sequence_error.max()),
        'alpha_and_protected_pixels_exact': True,
    }
    (p / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'cpu_reference'}, indent=2))
    print('max mean error', max(c['mean_rgb_error'] for c in checks))
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path, help='output directory of beauti-gpu-check')
    run(p.parse_args().directory)


if __name__ == '__main__':
    main()
