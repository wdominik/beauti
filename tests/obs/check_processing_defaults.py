# SPDX-License-Identifier: GPL-3.0-or-later
"""Check real OBS processing defaults and explicit selections without a camera."""

import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path


def run(cpu_bundle, tensorrt_bundle, harness, obs_runtime, fixture, output):
    """Check selection, CPU fallback, and explicit GPU failure in isolated OBS processes."""
    out = output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.pop('BEAUTI_ORT_PROFILE_DIR', None)
    env.pop('CUDA_VISIBLE_DEVICES', None)
    env['BEAUTI_OBS_RUNTIME'] = str(obs_runtime.resolve())
    env['PATH'] = str(obs_runtime.resolve() / 'bin/64bit') + ';' + str(Path(os.environ['SYSTEMROOT']) / 'System32')
    cases = [
        ('cpu-default', cpu_bundle, 'default', 2, 0, False),
        ('nvidia-default', tensorrt_bundle, 'default', 4, 0, False),
        ('nvidia-default-high-quality', tensorrt_bundle, 'default', 4, 1, False),
        ('saved-fp16', tensorrt_bundle, '3', 3, 0, False),
        ('saved-cpu', tensorrt_bundle, '2', 2, 0, False),
        ('hidden-gpu-default', tensorrt_bundle, 'default', 2, 0, True),
        ('hidden-gpu-explicit', tensorrt_bundle, '4', 4, 0, True),
    ]
    checks = []
    for name, bundle, selection, expected, quality, hidden in cases:
        case_env = dict(env, CUDA_VISIBLE_DEVICES='-1') if hidden else env
        path = out / (name + '.json')
        command = [
            str(harness.resolve()),
            str(bundle.resolve()),
            str(fixture.resolve()),
            str(path),
            '20',  # Allow the first TensorRT compilation; assertions still require processed output.
            str(quality),
            selection,
            '60',
            '50',
            '1280',
            '720',
            '0',
            '0',
            '0',
            'en-US',
            'Beauti',
            '50',
            '50',
        ]
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(command, env=case_env, stdout=log, stderr=log, check=True, timeout=90)
        log = (out / (name + '.log')).read_text()
        ui = json.loads(Path(str(path) + '.ui.json').read_text())
        assert ui['selected_backend'] == expected, ui
        assert ui['backend_has_user_value'] == (selection != 'default'), ui
        devices = next(p['items'] for p in ui['properties'] if p['name'] == 'native_backend')
        assert devices[0] == ('CPU' if bundle == cpu_bundle else 'NVIDIA Acceleration (Full Precision)'), devices
        rows = json.loads(path.read_text())['samples']
        processed = sum(row[0] >= 3 and row[3] > 0 for row in rows)
        if hidden and selection != 'default':
            assert '[beauti] native' in log and 'STATUS: Processing failed' in log, log[-2000:]
            assert processed == 0, 'An unavailable explicit GPU selection silently processed on another device'
        else:
            assert f'engine ready: backend={expected} quality={quality};' in log, log[-2000:]
            assert '[beauti] native' not in log and 'STATUS: 1280' in log, log[-2000:]
            assert processed > 0, name + ': no processed output'
        if hidden:
            assert 'acceleration unavailable (' in log and 'new filters default to the CPU' in log
            if selection == 'default':
                assert 'No acceleration found, running on the CPU' in log
        checks.append(
            {
                'case': name,
                'backend': expected,
                'quality': quality,
                'explicit_selection': selection != 'default',
                'gpu_hidden': hidden,
                'processed_samples': processed,
                'first_processed_seconds': next((row[0] for row in rows if row[3] > 0), None),
                'passed': True,
            }
        )
        print(name + ': passed', flush=True)
    report = {
        'passed': True,
        'checks': checks,
        'bundle_dll_sha256': {
            kind: hashlib.sha256((bundle / 'beauti.dll').read_bytes()).hexdigest()
            for kind, bundle in [('cpu', cpu_bundle), ('tensorrt', tensorrt_bundle)]
        },
        'scope': 'Actual unset OBS settings, explicit selections, and CPU fallback with CUDA_VISIBLE_DEVICES=-1. '
        'The driver remains installed; missing/old-driver machines and native Apple checks remain separate.',
    }
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('cpu-bundle', 'tensorrt-bundle', 'harness', 'obs-runtime', 'fixture', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    a = parser.parse_args()
    run(a.cpu_bundle, a.tensorrt_bundle, a.harness, a.obs_runtime, a.fixture, a.output)


if __name__ == '__main__':
    main()
