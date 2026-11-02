# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate native face inference, binary framing, bypasses, and provider profiles.

beauti-bench returns the blend map and the skin mask (see
tools/beauti_tools/bench/protocol.py); it does not composite pixels.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

import numpy as np

from beauti_tools.bench.protocol import BACKEND_IDS, REQUEST, pack_request, parse_responses

# (retouch, smooth) requests: bypass, blend only, mask only, both, bypass. The face region
# settles on a repeated frame within a few analyses, so "both" repeats until it has.
CONTROLS = [(0, 0), (100, 0), (0, 100), (60, 50), (60, 50), (60, 50), (60, 50), (0, 0)]
# ORT profile prefixes (model file stems) placed on the device by src/backends/backend.cpp.
TENSORRT_MODELS = ('skin_retouching', 'skin_segmenter')


def run(bundle, fixture, width, height, backend, output, quality='fast', profile=False):
    """Run the protocol check against the bundle; writes and returns the report."""
    bundle, out = bundle.resolve(), output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    original = fixture.read_bytes()
    size = width * height * 4
    assert len(original) == size, 'Fixture dimensions do not match'
    requests = b''.join(pack_request(width, height, r, s, i, original) for i, (r, s) in enumerate(CONTROLS, 1))
    darwin = sys.platform == 'darwin'
    command = [
        str(bundle / ('Contents/Resources/bin/beauti-bench' if darwin else 'beauti-bench.exe')),
        '--models',
        str(bundle / ('Contents/Resources/models' if darwin else 'data/models')),
        '--backend',
        backend,
        '--quality',
        quality,
        '--cache',
        str(out / 'cache'),
    ]
    env = os.environ.copy()
    if profile:
        env['BEAUTI_ORT_PROFILE_DIR'] = str(out / 'profiles')
    else:
        env.pop('BEAUTI_ORT_PROFILE_DIR', None)
    start = time.perf_counter()
    result = subprocess.run(command, input=requests, capture_output=True, timeout=300, env=env, check=False)
    (out / 'worker.log').write_bytes(result.stderr)
    result.check_returncode()
    elapsed = time.perf_counter() - start
    responses = parse_responses(result.stdout)
    assert len(responses) == len(CONTROLS), len(responses)
    frames = []
    payloads = []
    for response, (r, s) in zip(responses, CONTROLS, strict=True):
        i = response.sequence
        assert (response.width, response.height, response.status) == (width, height, 0), response
        assert response.used_backend == BACKEND_IDS[backend], response
        assert np.isfinite(response.milliseconds) and response.milliseconds >= 0, response
        assert (response.blend_size > 0) == (r > 0), ('blend map presence', r, response.blend_size)
        assert (response.mask_size > 0) == (s > 0), ('skin mask presence', s, response.mask_size)
        blend = np.frombuffer(response.blend, np.float32).reshape(-1, 4) if response.blend_size else None
        mask = np.frombuffer(response.mask, np.float32).reshape(-1, 4) if response.mask_size else None
        record = {
            'sequence': i,
            'retouch': r,
            'smooth': s,
            'milliseconds': response.milliseconds,
            'detect_ms': response.detect_ms,
            'infer_ms': response.infer_ms,
            'box': list(response.box),
            'blend_size': response.blend_size,
            'mask_size': response.mask_size,
        }
        if blend is not None:
            assert np.all(np.isfinite(blend)) and np.all(blend[:, 3] == 1), 'blend map alpha/finite'
            # The retouch model returns 0.5 where nothing changes; a face must move it.
            record['blend_mean_abs_deviation'] = float(np.abs(blend[:, :3] - 0.5).mean())
            assert record['blend_mean_abs_deviation'] > 0, 'Retouch inference returned a flat blend map'
            (out / f'frame-{i}.blend.f32').write_bytes(response.blend)
        if mask is not None:
            assert response.raw_mask, 'Native engines return the raw hard-region/confidence mask'
            assert np.all(np.isfinite(mask)) and np.all(mask[:, 3] == 1), 'mask alpha/finite'
            record['mask_coverage'] = float(np.mean(mask[:, 0] > 0.5))
            assert 0 < record['mask_coverage'] < 1, 'Skin mask covers nothing or everything'
            (out / f'frame-{i}.mask.f32').write_bytes(response.mask)
        payloads.append((response.box, response.blend, response.mask))
        frames.append(record)
    # Bypass frames carry no payload; a settled region gives identical maps for identical requests.
    assert payloads[0] == ((0, 0, 0, 0), b'', b'') and payloads[-1] == ((0, 0, 0, 0), b'', b''), 'bypass payload'
    assert payloads[-3] == payloads[-2], 'Repeated request produced different maps'
    invalid = subprocess.run(
        command,
        input=REQUEST.pack(0, 1, 1, 0, 0, 1),  # wrong magic, no pixels
        capture_output=True,
        timeout=120,
        check=False,
        env={k: v for k, v in env.items() if k != 'BEAUTI_ORT_PROFILE_DIR'},
    )
    assert invalid.returncode != 0 and b'Invalid frame header' in invalid.stderr
    coverage = {}
    if profile:
        for path in sorted((out / 'profiles').glob('*.json')):
            events = json.loads(path.read_text(encoding='utf-8'))
            nodes = [e for e in events if e.get('cat') == 'Node' and e.get('args', {}).get('provider')]
            counts = Counter(e['args']['provider'] for e in nodes)
            coverage[path.name] = dict(counts)
            # With TensorRT RTX the retouch model and the skin segmenter run entirely on the
            # device (docs/decisions/0022); detection and landmarks use the CPU.
            on_device = path.name.startswith(TENSORRT_MODELS) and backend.startswith('tensorrt')
            expected = 'NvTensorRTRTXExecutionProvider' if on_device else 'CPUExecutionProvider'
            assert counts[expected] > 0, (path.name, counts)
            if on_device:
                assert set(counts) == {expected}, ('CPU fallback inside a device model', path.name, counts)
        # Core AI models bypass ONNX Runtime; only the detector and FaceMesh are profiled then.
        assert len(coverage) == (2 if backend == 'coreai' else 4), coverage
    report = {
        'passed': True,
        'backend': backend,
        'quality': quality,
        'width': width,
        'height': height,
        'frames': frames,
        'wall_seconds': elapsed,
        'startup_and_protocol_overhead_seconds': elapsed - sum(f['milliseconds'] for f in frames) / 1000,
        'fixture_sha256': hashlib.sha256(original).hexdigest(),
        'bypass_without_payload': True,
        'repeat_exact': True,
        'invalid_header_rejected': True,
        'profile_node_events_by_provider': coverage,
        'scope': 'Native detection and Retouch/skin-mask inference as maps. '
        'Compositing and the four finishing stages are checked by the OBS GPU runner.',
    }
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--width', type=int, required=True)
    parser.add_argument('--height', type=int, required=True)
    parser.add_argument('--backend', choices=sorted(BACKEND_IDS), required=True)
    parser.add_argument('--quality', choices=['fast', 'full'], default='fast')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profile', action='store_true')
    a = parser.parse_args()
    run(a.bundle, a.fixture, a.width, a.height, a.backend, a.output, a.quality, a.profile)


if __name__ == '__main__':
    main()
