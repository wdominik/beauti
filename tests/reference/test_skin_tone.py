# SPDX-License-Identifier: GPL-3.0-or-later
"""Color-only contracts, including quantization, gamut and protected regions."""

import ctypes as ct
import sys
import unittest
from pathlib import Path

import cv2
import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from beauti_reference.skin_tone import equalize_skin_tone, mix_linear, tone_function

pytestmark = pytest.mark.needs_reference


def lab(rgb):
    return cv2.cvtColor(rgb.astype(np.float32) / 255, cv2.COLOR_RGB2Lab)


def luminance(rgb):
    c = rgb.astype(np.float64) / 255
    linear = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    return linear @ np.array([0.2126729, 0.7151522, 0.0721750])


def fixture(light=62):
    y, x = np.mgrid[:160, :192]
    values = np.empty((160, 192, 3), np.float32)
    values[..., 0] = light + 2 * np.sin(x * 1.7) * np.cos(y * 1.4) + 6 * x / 192
    values[..., 1] = 13 + 10 * np.exp(-((x - 50) ** 2 + (y - 80) ** 2) / 1600)
    values[..., 2] = 20 + 6 * np.exp(-((x - 140) ** 2 + (y - 80) ** 2) / 1300)
    rgb = np.rint(cv2.cvtColor(values, cv2.COLOR_Lab2RGB) * 255).astype(np.uint8)
    mask = np.ones((160, 192), np.float32)
    mask[:8] = mask[-8:] = 0
    mask[:, :8] = mask[:, -8:] = 0
    return rgb, mask


class SkinToneTests(unittest.TestCase):
    def test_bypass_protection_and_input_immutability(self):
        rgb, mask = fixture()
        copy = rgb.copy()
        mask_copy = mask.copy()
        for skin, amount in [(mask, 0), (np.zeros_like(mask), 1)]:
            out, _ = equalize_skin_tone(rgb, skin, amount)
            np.testing.assert_array_equal(out, rgb)
        out, _ = equalize_skin_tone(rgb, mask)
        np.testing.assert_array_equal(out[mask == 0], rgb[mask == 0])
        np.testing.assert_array_equal(rgb, copy)
        np.testing.assert_array_equal(mask, mask_copy)

    def test_regional_color_reduction_without_luminance_blur(self):
        for light in [24, 45, 62, 78]:
            with self.subTest(light=light):
                rgb, mask = fixture(light)
                out, stats = equalize_skin_tone(rgb, mask)
                before, after = lab(rgb), lab(out)
                inside = mask > 0.9
                self.assertLess(np.std(after[..., 1][inside]), np.std(before[..., 1][inside]) * 0.65)
                # Output RGB8 quantization is the only intended luminance change.
                self.assertLess(np.max(np.abs(after[..., 0] - before[..., 0])), 0.45)
                self.assertLess(np.max(np.abs(luminance(out) - luminance(rgb))), 0.0045)

                def detail(l):
                    return l - cv2.GaussianBlur(l, (0, 0), 1.2)

                ratio = np.std(detail(after[..., 0])[inside]) / np.std(detail(before[..., 0])[inside])
                self.assertTrue(0.96 < ratio < 1.04, ratio)
                self.assertGreater(stats['mean_chroma_correction'], 0.5)

    def test_flat_colors_and_small_odd_sizes(self):
        for shape in [(2, 2), (7, 13), (31, 81)]:
            for color in [(0, 0, 0), (255, 255, 255), (100, 57, 38), (214, 161, 139), (127, 127, 127)]:
                rgb = np.full((*shape, 3), color, np.uint8)
                out, _ = equalize_skin_tone(rgb, np.ones(shape, np.float32))
                np.testing.assert_array_equal(out, rgb)

    def test_extreme_colors_and_gamut_limiting_preserve_luminance(self):
        rng = np.random.default_rng(42)
        rgb = rng.integers(0, 256, (63, 81, 3), dtype=np.uint8)
        rgb[0, :6] = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (0, 0, 0), (255, 255, 255), (255, 255, 0)]
        out, _ = equalize_skin_tone(rgb, np.ones(rgb.shape[:2], np.float32))
        self.assertLess(np.max(np.abs(luminance(out) - luminance(rgb))), 0.0045)
        np.testing.assert_array_equal(out[0, 3:5], rgb[0, 3:5])

    def test_strength_is_monotonic_and_linear_blend_preserves_luminance(self):
        rgb, mask = fixture()
        full, _ = equalize_skin_tone(rgb, mask)
        previous = -1
        for strength in [0, 0.25, 0.5, 0.75, 1]:
            out = mix_linear(rgb, full, strength)
            distance = np.abs(out.astype(float) - rgb).mean()
            self.assertGreaterEqual(distance, previous)
            previous = distance
            self.assertLess(np.max(np.abs(luminance(out) - luminance(rgb))), 0.006)
        np.testing.assert_array_equal(mix_linear(rgb, full, 0), rgb)
        np.testing.assert_array_equal(mix_linear(rgb, full, 1), full)

    def test_insufficient_confident_skin_is_noop(self):
        rgb, mask = fixture()
        mask[:] = 0.2
        out, _ = equalize_skin_tone(rgb, mask)
        np.testing.assert_array_equal(out, rgb)

    def test_invalid_input_and_in_place_output(self):
        rgb, mask = fixture()
        for amount in [-1, 1.1, float('nan'), float('inf')]:
            with self.assertRaises(ValueError):
                equalize_skin_tone(rgb, mask, amount)
        for value in [-0.01, 1.01, float('nan')]:
            bad = mask.copy()
            bad[0, 0] = value
            with self.assertRaises(ValueError):
                equalize_skin_tone(rgb, bad)
        for bad in [rgb.astype(float), rgb[..., 0]]:
            with self.assertRaises(ValueError):
                equalize_skin_tone(bad, mask)
        expected, _ = equalize_skin_tone(rgb, mask)
        in_place = rgb.copy()
        pointer = in_place.ctypes.data_as(ct.POINTER(ct.c_uint8))
        err = ct.create_string_buffer(512)
        self.assertEqual(
            tone_function()(
                pointer,
                mask.ctypes.data_as(ct.POINTER(ct.c_float)),
                rgb.shape[1],
                rgb.shape[0],
                1,
                pointer,
                None,
                err,
                len(err),
            ),
            0,
        )
        np.testing.assert_array_equal(in_place, expected)


if __name__ == '__main__':
    unittest.main()
