# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare two native engines on motion, sizes, alpha, bypass, and no-face frames.

Both bundles' beauti-bench workers receive the same 12-frame corpus; detection
boxes and the returned blend maps / skin masks are compared texel by texel.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import cv2
import numpy as np

from beauti_tools.bench.protocol import BACKEND_IDS, pack_request, parse_responses

# Map values are in [0, 1]; the tolerances mirror the former 8-bit limits (2/255, 0.01/255).
MAX_ERROR = 2 / 255
MEAN_ERROR = 0.01 / 255


def run(
    reference_bundle, candidate_bundle, fixture, output, backend, quality='fast', exact=False, reference_backend=None
):
    """Compare both workers on the corpus; writes and returns the report, failing on a mismatch."""
    out = output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    base = np.fromfile(fixture, np.uint8).reshape(720, 1280, 4)
    cases = [('original', base, 60, 50)]
    for offset in (-80, -31, 35, 83):
        moved = cv2.warpAffine(
            base, np.float32([[1, 0, offset], [0, 1, 17]]), (1280, 720), borderValue=(65, 70, 75, 255)
        )
        cases.append((f'motion-{offset}', moved, 60, 50))
    for width, height in ((960, 540), (641, 361), (1280, 720)):
        cases.append((f'size-{width}-{height}', cv2.resize(base, (width, height)), 100, 100))
    alpha = base.copy()
    alpha[..., 3] = np.where(np.indices(base.shape[:2])[1] < 50, 0, 127)
    alpha[..., :3] = np.rint(base[..., :3].astype(float) * alpha[..., 3:] / 255).astype(np.uint8)
    cases += [
        ('alpha', alpha, 60, 50),
        ('zero-strength', base, 0, 0),
        ('no-face', np.full_like(base, (80, 96, 112, 255)), 60, 50),
        ('repeat-original', base, 60, 50),
    ]
    requests = b''.join(
        pack_request(f.shape[1], f.shape[0], r, s, i, f.tobytes()) for i, (_, f, r, s) in enumerate(cases, 1)
    )
    env = os.environ.copy()
    env.pop('BEAUTI_ORT_PROFILE_DIR', None)
    if sys.platform == 'win32':
        env['PATH'] = str(Path(os.environ['SYSTEMROOT']) / 'System32')
    outputs = {}
    hashes = {}
    for label, bundle in [('reference', reference_bundle.resolve()), ('candidate', candidate_bundle.resolve())]:
        exe = bundle / ('Contents/Resources/bin/beauti-bench' if sys.platform == 'darwin' else 'beauti-bench.exe')
        selected_backend = (reference_backend or backend) if label == 'reference' else backend
        command = [
            str(exe),
            '--models',
            str(bundle / ('Contents/Resources/models' if sys.platform == 'darwin' else 'data/models')),
            '--backend',
            selected_backend,
            '--quality',
            quality,
            '--cache',
            str(out / 'cache'),
        ]
        run = subprocess.run(command, input=requests, capture_output=True, timeout=300, env=env, check=False)
        (out / (label + '.log')).write_bytes(run.stderr)
        run.check_returncode()
        hashes[label] = hashlib.sha256(exe.read_bytes()).hexdigest()
        responses = parse_responses(run.stdout)
        assert len(responses) == len(cases), len(responses)
        for response, (_, f, _, _) in zip(responses, cases, strict=True):
            assert (response.width, response.height) == (f.shape[1], f.shape[0]), response
            assert response.used_backend == BACKEND_IDS[selected_backend], response
        outputs[label] = responses
    records = []
    for i, (name, _, r, s) in enumerate(cases):
        ref, cand = outputs['reference'][i], outputs['candidate'][i]
        errors = {}
        for kind in ('blend', 'mask'):
            a_bytes, b_bytes = getattr(ref, kind), getattr(cand, kind)
            if len(a_bytes) != len(b_bytes):
                errors[kind] = {'max': float('inf'), 'mean': float('inf'), 'size_match': False}
                continue
            if not a_bytes:
                errors[kind] = {'max': 0.0, 'mean': 0.0, 'size_match': True}
                continue
            delta = np.abs(np.frombuffer(a_bytes, np.float32) - np.frombuffer(b_bytes, np.float32))
            errors[kind] = {'max': float(delta.max()), 'mean': float(delta.mean()), 'size_match': True}
        record = {
            'case': name,
            'status_match': ref.status == cand.status,
            'box_reference': list(ref.box),
            'box_candidate': list(cand.box),
            'blend': errors['blend'],
            'mask': errors['mask'],
            'reference_ms': ref.milliseconds,
            'candidate_ms': cand.milliseconds,
        }
        records.append(record)
        if r == s == 0 or ref.status == 1:
            assert not cand.blend and not cand.mask, 'Bypass or no-face frame returned maps'
    all_maps_exact = all(e['max'] == 0 for rec in records for e in (rec['blend'], rec['mask']))
    limit = 0 if exact else MAX_ERROR
    passed = all(
        rec['status_match']
        and rec['box_reference'] == rec['box_candidate']
        and all(e['size_match'] and e['max'] <= limit and e['mean'] <= MEAN_ERROR for e in (rec['blend'], rec['mask']))
        for rec in records
    )
    report = {
        'passed': passed,
        'backend': backend,
        'quality': quality,
        'all_maps_exact': all_maps_exact,
        'reference_backend': reference_backend or backend,
        'worker_sha256': hashes,
        'cases': records,
    }
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    assert passed, 'Candidate changed detection or exceeded map tolerances'
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('reference-bundle', 'candidate-bundle', 'fixture', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--backend', choices=sorted(BACKEND_IDS), required=True)
    p.add_argument('--quality', choices=['fast', 'full'], default='fast')
    p.add_argument('--exact', action='store_true')
    p.add_argument(
        '--reference-backend', choices=sorted(BACKEND_IDS), help='Backend for the reference worker when it differs'
    )
    a = p.parse_args()
    run(a.reference_bundle, a.candidate_bundle, a.fixture, a.output, a.backend, a.quality, a.exact, a.reference_backend)


if __name__ == '__main__':
    main()
