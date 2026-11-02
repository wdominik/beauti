# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare real processed-frame delivery in isolated libobs runs, without a camera."""

import argparse
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import sys
from pathlib import Path

import live_stats
import numpy as np


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'harness', 'fixture', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument(
        '--obs-runtime',
        type=Path,
        default=Path('/Applications/OBS.app/Contents/Frameworks') if sys.platform == 'darwin' else None,
    )
    p.add_argument('--warmup-seconds', type=float, default=3)
    p.add_argument(
        '--reference-bundle',
        type=Path,
        help='Interleave the unchanged reference with the candidate in reversed repeat order',
    )
    p.add_argument('--width', type=int, default=3840)
    p.add_argument('--height', type=int, default=2160)
    p.add_argument('--seconds', type=int, default=20)
    p.add_argument('--repeats', type=int, default=2)
    p.add_argument(
        '--backends',
        nargs='+',
        choices=['cpu', 'coreai', 'tensorrt', 'tensorrt-fp32'],
        default=['coreai'] if sys.platform == 'darwin' else ['tensorrt', 'tensorrt-fp32'],
    )
    p.add_argument('--static', action='store_true')
    p.add_argument('--quality', choices=['fast', 'full'], default='fast')
    p.add_argument('--canvas-fps', type=int, choices=range(1, 121), default=30, metavar='1-120')
    p.add_argument('--eyes', type=int, default=0, help='Eyes strength')
    p.add_argument(
        '--analysis-fps', type=int, choices=range(61), metavar='0-60', help='Plugin target frame rate; 0: automatic'
    )
    a = p.parse_args()
    if a.obs_runtime is None:
        p.error('--obs-runtime is required on Windows')
    if a.repeats < 1 or a.seconds <= a.warmup_seconds or a.warmup_seconds < 0:
        p.error('Use positive repeats and a run longer than the nonnegative warmup')
    if a.fixture.stat().st_size != a.width * a.height * 4:
        p.error('Fixture dimensions do not match its byte length')
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.pop('BEAUTI_ORT_PROFILE_DIR', None)
    env.pop('BEAUTI_TIMING', None)
    for key in ('BEAUTI_FIXTURE_FPS', 'BEAUTI_FIXTURE_CUT', 'BEAUTI_CONTINUITY_TEST'):
        env.pop(key, None)
    env['BEAUTI_OBS_RUNTIME'] = str(a.obs_runtime.resolve())
    env['BEAUTI_CANVAS_FPS'] = str(a.canvas_fps)
    if a.analysis_fps is not None:
        env['BEAUTI_FIXTURE_FPS'] = str(a.analysis_fps)
    if sys.platform == 'win32':
        env['PATH'] = (
            str(a.obs_runtime.resolve() / 'bin/64bit') + ';' + str(Path(os.environ['SYSTEMROOT']) / 'System32')
        )
    results = []
    bundles = {'candidate': a.bundle}
    if a.reference_bundle:
        bundles = {'reference': a.reference_bundle, **bundles}
    variants = [(label, backend) for backend in a.backends for label in bundles]
    for repeat in range(a.repeats):
        order = variants if repeat % 2 == 0 else list(reversed(variants))
        for label, backend in order:
            bundle = bundles[label]
            name = f'{repeat + 1}-{label}-{backend}'
            path = out / (name + '.json')
            command = [
                str(a.harness.resolve()),
                str(bundle.resolve()),
                str(a.fixture.resolve()),
                str(path),
                str(a.seconds),
                '0' if a.quality == 'fast' else '1',
                str({'coreai': 0, 'cpu': 2, 'tensorrt': 3, 'tensorrt-fp32': 4}[backend]),
                '60',
                '50',
                str(a.width),
                str(a.height),
                '0' if a.static else '1',
                '0',
                '0',
                'en-US',
                'Beauti',
                '50',
                '50',
                str(a.eyes),
            ]
            with (out / (name + '.log')).open('w') as log:
                subprocess.run(command, stdout=log, stderr=log, env=env, check=True, timeout=a.seconds + 300)
            log = (out / (name + '.log')).read_text()
            assert 'engine ready:' in log and f'STATUS: {a.width}' in log, log[-2000:]
            assert '[beauti] native' not in log, log[-2000:]
            data = json.loads(path.read_text())
            rows = np.asarray(data['samples'])
            first_processed = next((row[0] for row in data['samples'] if row[3] > 0), None)
            rows = rows[rows[:, 0] >= a.warmup_seconds]
            assert len(rows) > 1, 'Not enough video samples'
            duration = float(rows[-1, 0] - rows[0, 0])
            processed = rows[rows[:, 3] > 0]
            assert len(processed), 'No processed frames'
            ages = (processed[:, 2] - processed[:, 1]) * 1000 / a.canvas_fps
            timings = [
                dict(zip(['total_ms', 'detect_ms', 'infer_ms', 'post_ms'], map(float, m)))
                for m in re.findall(r'total=([\d.]+)ms detect=([\d.]+) infer=([\d.]+) post=([\d.]+)', log)
            ]
            record = {
                'variant': label,
                'provider': (
                    'TensorRT RTX' if (bundle / 'onnxruntime_providers_nv_tensorrt_rtx.dll').exists() else backend
                ),
                'backend': backend,
                'repeat': repeat + 1,
                'sample_seconds': duration,
                'first_processed_seconds': first_processed,
                'distinct_processed_fps': max(0, len(np.unique(processed[:, 1])) - 1) / duration,
                'output_fps': (len(rows) - 1) / duration,
                'passthrough_frames': int(np.count_nonzero(rows[:, 3] == 0)),
                'median_age_ms': float(np.median(ages)),
                'p95_age_ms': float(np.quantile(ages, 0.95)),
                'render_ms': data['render_ms'],
                'lagged_after_warmup': data['lagged_frames_after_warmup'],
                'sparse_worker_samples': timings,
                'live_stats': next(iter(live_stats.parse(log).values()), None),
            }
            results.append(record)
            print(json.dumps(record), flush=True)
            report = {
                'bundle_module_sha256': {
                    label: hashlib.sha256(
                        (bundle / ('Contents/MacOS/beauti' if sys.platform == 'darwin' else 'beauti.dll')).read_bytes()
                    ).hexdigest()
                    for label, bundle in bundles.items()
                },
                'width': a.width,
                'height': a.height,
                'motion': not a.static,
                'canvas_fps': a.canvas_fps,
                'analysis_fps': 30 if a.analysis_fps is None else a.analysis_fps,
                'analysis_long_side': 1280,
                'quality': 'Performance' if a.quality == 'fast' else 'High Quality',
                'controls': [60, 50, 50, 50],
                'eyes': a.eyes,
                'warmup_excluded_seconds': a.warmup_seconds,
                'host': {'system': platform.system(), 'release': platform.release(), 'machine': platform.machine()},
                'fixture_sha256': hashlib.sha256(a.fixture.read_bytes()).hexdigest(),
                'runs': results,
                'median_distinct_processed_fps': {
                    label: {
                        b: statistics.median(
                            r['distinct_processed_fps'] for r in results if r['variant'] == label and r['backend'] == b
                        )
                        for b in a.backends
                        if any(r['variant'] == label and r['backend'] == b for r in results)
                    }
                    for label in bundles
                },
            }
            (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
