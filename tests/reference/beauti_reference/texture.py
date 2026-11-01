# SPDX-License-Identifier: GPL-3.0-or-later
"""CPU reference adapter for Beauti's independent scale-selective Texture finish."""

from __future__ import annotations

import ctypes as ct
from functools import lru_cache

import numpy as np

from .effects_library import library


@lru_cache(maxsize=1)
def texture_function():
    try:
        function = library().beauti_texture_rgb
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


def reduce_skin_texture(roi: np.ndarray, mask: np.ndarray, amount=1.0):
    if roi.dtype != np.uint8 or roi.ndim != 3 or roi.shape[2] != 3 or mask.shape != roi.shape[:2]:
        raise ValueError('Expected an RGB8 reference and a matching skin mask.')
    source = np.ascontiguousarray(roi)
    skin = np.ascontiguousarray(mask, dtype=np.float32)
    out = np.empty_like(source)
    metrics = (ct.c_float * 4)()
    error = ct.create_string_buffer(512)
    result = texture_function()(
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
        zip(('base_spacing', 'levels', 'mean_lightness_correction', 'processing_ms'), (round(v, 3) for v in metrics))
    )
