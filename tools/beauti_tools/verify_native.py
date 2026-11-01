# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the installed native worker protocol without external photos or Python ML packages."""

import argparse
import json
import subprocess
import sys
from pathlib import Path

from .bench.protocol import BACKEND_IDS, pack_request, parse_responses


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('bundle', type=Path)
    p.add_argument('--backend', choices=sorted(BACKEND_IDS), default='cpu')
    a = p.parse_args()
    windows = sys.platform == 'win32'
    bundle = a.bundle.resolve()
    res = bundle / ('data' if windows else 'Contents/Resources')
    exe = bundle / ('beauti-bench.exe' if windows else 'Contents/Resources/bin/beauti-bench')
    pixels = bytes([80, 96, 112, 255]) * 64 * 64
    # Frame 1 requests nothing (bypass); frame 2 requests both maps on a face-free image.
    frames = pack_request(64, 64, 0, 0, 1, pixels) + pack_request(64, 64, 60, 50, 2, pixels)
    result = subprocess.run(
        [str(exe), '--models', str(res / 'models'), '--backend', a.backend],
        input=frames,
        capture_output=True,
        timeout=120,
        check=True,
    )
    responses = parse_responses(result.stdout)
    assert [(r.sequence, r.status, r.width, r.height) for r in responses] == [(1, 0, 64, 64), (2, 1, 64, 64)], responses
    assert all(r.blend_size == 0 and r.mask_size == 0 and not r.blend and not r.mask for r in responses), responses
    assert all(r.used_backend == BACKEND_IDS[a.backend] for r in responses), responses
    print(
        json.dumps(
            {
                'passed': True,
                'backend': a.backend,
                'bypass_without_payload': True,
                'no_face_without_payload': True,
                'binary_protocol_frames': 2,
                'scope': 'Synthetic no-face smoke test of the maps protocol; '
                'not a face-quality or performance benchmark.',
            },
            indent=2,
        )
    )


if __name__ == '__main__':
    main()
