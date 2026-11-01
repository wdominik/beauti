# SPDX-License-Identifier: GPL-3.0-or-later
"""CPU reference-app adapter for Beauti's portable C++ reference effects core."""

from __future__ import annotations

import ctypes as ct
import sys
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


@lru_cache(maxsize=1)
def library():
    name = (
        'beauti-effects.dll'
        if sys.platform == 'win32'
        else 'libbeauti-effects.dylib'
        if sys.platform == 'darwin'
        else 'libbeauti-effects.so'
    )
    path = ROOT / 'build' / 'effects' / 'reference' / name
    if not path.is_file():
        raise RuntimeError('The reference effects library is missing. Run uv run beauti-build-reference first.')
    lib = ct.CDLL(str(path))
    lib.beauti_effects_api_version.restype = ct.c_int
    if lib.beauti_effects_api_version() != 1:
        raise RuntimeError('The reference effects library needs to be rebuilt.')
    return lib
