# SPDX-License-Identifier: GPL-3.0-or-later
"""CPU reference adapter for Beauti's independent, color-only C++ Skin Tone effect."""

from __future__ import annotations

import ctypes as ct
from functools import lru_cache

import numpy as np

from .effects_library import library


@lru_cache(maxsize=1)
def tone_function():
    try:
        function = library().beauti_skin_tone_rgb
    except AttributeError as exc:
        raise RuntimeError('The reference effects library needs to be rebuilt.') from exc
    function.argtypes = [
        ct.POINTER(ct.c_uint8),
        ct.POINTER(ct.c_float),
        ct.c_int,
        ct.c_int,
        ct.c_float,
        ct.POINTER(ct.c_uint8),
        ct.POINTER(ct.c_float),
        ct.c_char_p,
        ct.c_size_t,
    ]
    function.restype = ct.c_int
    return function


def equalize_skin_tone(roi: np.ndarray, mask: np.ndarray, amount=1.0):
    if roi.dtype != np.uint8 or roi.ndim != 3 or roi.shape[2] != 3 or mask.shape != roi.shape[:2]:
        raise ValueError('Expected an RGB8 reference and a matching skin mask.')
    source = np.ascontiguousarray(roi)
    skin = np.ascontiguousarray(mask, dtype=np.float32)
    out = np.empty_like(source)
    metrics = (ct.c_float * 4)()
    error = ct.create_string_buffer(512)
    result = tone_function()(
        source.ctypes.data_as(ct.POINTER(ct.c_uint8)),
        skin.ctypes.data_as(ct.POINTER(ct.c_float)),
        source.shape[1],
        source.shape[0],
        amount,
        out.ctypes.data_as(ct.POINTER(ct.c_uint8)),
        metrics,
        error,
        len(error),
    )
    if result:
        raise ValueError(error.value.decode('utf-8', errors='replace'))
    return out, dict(
        zip(('target_a', 'target_b', 'mean_chroma_correction', 'processing_ms'), (round(v, 3) for v in metrics))
    )


def mix_linear(before, after, strength):
    """Blend endpoints in linear sRGB to retain luminance between slider stops."""
    if strength == 0:
        return before.copy()
    if strength == 1:
        return after.copy()
    values = np.arange(256, dtype=np.float64) / 255
    lut = np.where(values <= 0.04045, values / 12.92, ((values + 0.055) / 1.055) ** 2.4)
    linear = lut[before] * (1 - strength) + lut[after] * strength
    encoded = np.where(linear <= 0.0031308, 12.92 * linear, 1.055 * linear ** (1 / 2.4) - 0.055)
    return np.rint(np.clip(encoded, 0, 1) * 255).astype(np.uint8)
