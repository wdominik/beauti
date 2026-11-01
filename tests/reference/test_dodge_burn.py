# SPDX-License-Identifier: GPL-3.0-or-later
"""Image and sequential-pipeline contracts for Micro Dodge & Burn."""

import unittest

import cv2
import numpy as np
import pytest

from beauti_reference.dodge_burn import micro_dodge_burn

pytestmark = pytest.mark.needs_reference


def lab(rgb):
    return cv2.cvtColor(rgb.astype(np.float32) / 255, cv2.COLOR_RGB2Lab)


def fixture():
    y, x = np.indices((256, 288))
    values = np.empty((256, 288, 3), np.float32)
    values[..., 0] = (
        58
        + 8 * x / 288
        - 8 * np.exp(-((x - 80) ** 2 + (y - 128) ** 2) / 72)
        + 6 * np.exp(-((x - 190) ** 2 + (y - 128) ** 2) / 72)
        + 0.8 * ((x + y) % 2 * 2 - 1)
    )
    values[..., 1] = 14
    values[..., 2] = 19
    rgb = np.rint(cv2.cvtColor(values, cv2.COLOR_Lab2RGB) * 255).astype(np.uint8)
    mask = np.ones(rgb.shape[:2], np.float32)
    return rgb, mask


class DodgeBurnTests(unittest.TestCase):
    def test_lightens_dark_and_darkens_bright_irregularities(self):
        rgb, mask = fixture()
        out, stats = micro_dodge_burn(rgb, mask)
        a, b = lab(rgb)[..., 0], lab(out)[..., 0]
        self.assertGreater(np.mean(b[125:132, 77:84] - a[125:132, 77:84]), 0.7)
        self.assertLess(np.mean(b[125:132, 187:194] - a[125:132, 187:194]), -0.5)
        self.assertGreater(stats['dodged_fraction'], 0)
        self.assertGreater(stats['burned_fraction'], 0)
        self.assertLess(np.max(abs(a - b)), 4.4)

    def test_retains_fine_texture_broad_shading_and_chroma(self):
        rgb, mask = fixture()
        out, _ = micro_dodge_burn(rgb, mask)
        a, b = lab(rgb), lab(out)

        def fine(l):
            return l - cv2.GaussianBlur(l, (0, 0), 0.7)

        area = np.s_[32:-32, 32:-32]
        ratio = np.std(fine(b[..., 0])[area]) / np.std(fine(a[..., 0])[area])
        self.assertTrue(0.94 < ratio < 1.06, ratio)
        self.assertLess(np.quantile(np.linalg.norm(a[..., 1:] - b[..., 1:], axis=-1), 0.99), 0.8)
        self.assertLess(np.max(abs(a[:80, 30:-30, 0] - b[:80, 30:-30, 0])), 0.5)

    def test_exact_bypass_mask_exclusion_and_input_preservation(self):
        rgb, mask = fixture()
        saved = rgb.copy()
        for amount, m in [(0, mask), (1, mask * 0)]:
            out, _ = micro_dodge_burn(rgb, m, amount)
            np.testing.assert_array_equal(out, rgb)
        mask[:, 130:155] = 0
        altered = rgb.copy()
        altered[:, 130:155] = [0, 255, 0]
        a, _ = micro_dodge_burn(rgb, mask)
        b, _ = micro_dodge_burn(altered, mask)
        np.testing.assert_array_equal(a[mask == 0], rgb[mask == 0])
        np.testing.assert_array_equal(a[mask > 0], b[mask > 0])
        np.testing.assert_array_equal(rgb, saved)

    def test_constant_images_and_strong_edges(self):
        for shape in [(2, 3), (31, 37), (128, 128)]:
            for color in [(0, 0, 0), (255, 255, 255), (160, 100, 70), (255, 0, 0)]:
                rgb = np.full((*shape, 3), color, np.uint8)
                out, _ = micro_dodge_burn(rgb, np.ones(shape, np.float32))
                np.testing.assert_array_equal(out, rgb)
        rgb = np.full((128, 128, 3), 75, np.uint8)
        rgb[:, 64:] = 195
        out, _ = micro_dodge_burn(rgb, np.ones((128, 128), np.float32))
        self.assertLessEqual(np.max(abs(out.astype(int) - rgb.astype(int))), 2)

    def test_strength_monotonic_in_lightness_and_translation(self):
        rgb, mask = fixture()
        a = lab(rgb)[..., 0]
        previous = 0
        for amount in [0, 0.25, 0.5, 0.75, 1]:
            out, _ = micro_dodge_burn(rgb, mask, amount)
            distance = np.mean(abs(lab(out)[..., 0] - a))
            self.assertGreaterEqual(distance, previous)
            previous = distance
        shifted, _ = micro_dodge_burn(np.roll(rgb, 1, axis=1), mask)
        out, _ = micro_dodge_burn(rgb, mask)
        np.testing.assert_array_equal(shifted[32:-32, 32:-32], np.roll(out, 1, axis=1)[32:-32, 32:-32])

    def test_invalid_parameters(self):
        rgb, mask = fixture()
        for amount in [-1, 2, float('nan'), float('inf')]:
            with self.assertRaises(ValueError):
                micro_dodge_burn(rgb, mask, amount)
        for value in [-1, 2, float('nan'), float('inf')]:
            m = mask.copy()
            m[0, 0] = value
            with self.assertRaises(ValueError):
                micro_dodge_burn(rgb, m)


if __name__ == '__main__':
    unittest.main()
