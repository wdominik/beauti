# SPDX-License-Identifier: GPL-3.0-or-later
"""Observable image contracts for Texture; no extra neural-model inference."""

import unittest

import cv2
import numpy as np
import pytest

from beauti_reference.texture import reduce_skin_texture

pytestmark = pytest.mark.needs_reference


class TextureTests(unittest.TestCase):
    def setUp(self):
        y, x = np.indices((192, 224))
        values = 135 + 16 * np.cos(2 * np.pi * x / 200) + 5 * np.sin(2 * np.pi * x / 6) * np.cos(2 * np.pi * y / 6)
        self.rgb = np.rint(values[..., None] + np.array([25, 0, -15])).astype(np.uint8)
        self.mask = np.ones(self.rgb.shape[:2], np.float32)

    def test_exact_bypass_exclusions_and_no_mutation(self):
        original = self.rgb.copy()
        for amount, mask in [(0, self.mask), (1, self.mask * 0)]:
            out, _ = reduce_skin_texture(self.rgb, mask, amount)
            np.testing.assert_array_equal(out, self.rgb)
        self.mask[40:110, 50:90] = 0
        out, _ = reduce_skin_texture(self.rgb, self.mask)
        np.testing.assert_array_equal(out[self.mask == 0], self.rgb[self.mask == 0])
        np.testing.assert_array_equal(self.rgb, original)

    def test_reduces_small_relief_but_retains_broad_shading(self):
        out, _ = reduce_skin_texture(self.rgb, self.mask)
        before = cv2.cvtColor(self.rgb.astype(np.float32) / 255, cv2.COLOR_RGB2Lab)[..., 0]
        after = cv2.cvtColor(out.astype(np.float32) / 255, cv2.COLOR_RGB2Lab)[..., 0]
        area = np.s_[32:-32, 32:-32]
        # Independent spatial measurements, not a reimplementation of the filter.
        low_before = cv2.GaussianBlur(before, (0, 0), 9)
        low_after = cv2.GaussianBlur(after, (0, 0), 9)
        energy_before = np.std((before - low_before)[area])
        energy_after = np.std((after - low_after)[area])
        self.assertLess(energy_after, energy_before * 0.65)
        self.assertGreater(energy_after, energy_before * 0.10)
        self.assertLess(np.max(np.abs(low_after - low_before)[area]), 0.8)

    def test_keeps_strong_edges_and_does_not_import_masked_colors(self):
        image = np.full((128, 128, 3), 80, np.uint8)
        image[:, 64:] = 190
        out, _ = reduce_skin_texture(image, np.ones((128, 128), np.float32))
        self.assertLessEqual(np.max(abs(out.astype(int) - image.astype(int))), 1)
        mask = self.mask.copy()
        mask[:, 100:120] = 0
        changed = self.rgb.copy()
        changed[:, 100:120] = [0, 255, 0]
        a, _ = reduce_skin_texture(self.rgb, mask)
        b, _ = reduce_skin_texture(changed, mask)
        np.testing.assert_array_equal(a[mask > 0], b[mask > 0])

    def test_preserves_lab_chroma_within_rgb8_rounding(self):
        out, _ = reduce_skin_texture(self.rgb, self.mask)
        before = cv2.cvtColor(self.rgb.astype(np.float32) / 255, cv2.COLOR_RGB2Lab)
        after = cv2.cvtColor(out.astype(np.float32) / 255, cv2.COLOR_RGB2Lab)
        chroma = np.linalg.norm(after[..., 1:] - before[..., 1:], axis=-1)
        self.assertLess(np.quantile(chroma, 0.99), 0.8)
        self.assertGreater(np.mean(abs(after[..., 0] - before[..., 0])), 0.15)

    def test_smooth_strength_and_translation(self):
        full, _ = reduce_skin_texture(self.rgb, self.mask)
        previous = self.rgb.astype(int)
        for strength in [0.2, 0.4, 0.6, 0.8, 1]:
            out, _ = reduce_skin_texture(self.rgb, self.mask, strength)
            expected = self.rgb.astype(float) * (1 - strength) + full.astype(float) * strength
            self.assertLessEqual(np.max(abs(out - expected)), 1)
            self.assertTrue(np.all((out.astype(int) - previous) * (full.astype(int) - self.rgb.astype(int)) >= 0))
            previous = out.astype(int)
        shifted, _ = reduce_skin_texture(np.roll(self.rgb, 1, axis=1), self.mask)
        np.testing.assert_array_equal(np.roll(full, 1, axis=1)[40:-40, 40:-40], shifted[40:-40, 40:-40])

    def test_constant_extreme_colors_and_small_odd_images(self):
        for shape in [(2, 3), (31, 33), (73, 81)]:
            for color in [(0, 0, 0), (255, 255, 255), (255, 0, 0), (180, 90, 45)]:
                rgb = np.full((*shape, 3), color, np.uint8)
                out, _ = reduce_skin_texture(rgb, np.ones(shape, np.float32))
                np.testing.assert_array_equal(out, rgb)

    def test_invalid_parameters(self):
        for amount in [-1, 2, float('nan'), float('inf')]:
            with self.assertRaises(ValueError):
                reduce_skin_texture(self.rgb, self.mask, amount)
        for value in [-1, 2, float('nan'), float('inf')]:
            mask = self.mask.copy()
            mask[0, 0] = value
            with self.assertRaises(ValueError):
                reduce_skin_texture(self.rgb, mask)


if __name__ == '__main__':
    unittest.main()
