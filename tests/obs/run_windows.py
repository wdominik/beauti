# SPDX-License-Identifier: GPL-3.0-or-later
"""Run isolated Windows OBS fixtures, slider checks, and measured frame delivery.

The fixture directory contains face-720p.rgba and face-4k.rgba. No camera,
OBS application, scene collection, or installed plugin is changed.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('cpu-bundle', 'tensorrt-bundle', 'harness', 'obs-runtime', 'fixtures', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.pop('BEAUTI_ORT_PROFILE_DIR', None)
    for key in (
        'BEAUTI_TIMING',
        'BEAUTI_FIXTURE_FPS',
        'BEAUTI_FIXTURE_CUT',
        'BEAUTI_CONTINUITY_TEST',
        'BEAUTI_FIXTURE_CONTROL_START_SECONDS',
    ):
        env.pop(key, None)
    env['BEAUTI_OBS_RUNTIME'] = str(args.obs_runtime.resolve())
    env['PATH'] = str(args.obs_runtime.resolve() / 'bin/64bit') + ';' + str(Path(os.environ['SYSTEMROOT']) / 'System32')
    records = {}

    def vram():
        result = subprocess.run(
            ['nvidia-smi', '--query-gpu=memory.used', '--format=csv,noheader,nounits'],
            capture_output=True,
            text=True,
            env=env,
            timeout=10,
            check=False,
        )
        return int(result.stdout.splitlines()[0]) if result.returncode == 0 else None

    def run(name, backend=3, mode=0, controls=(60, 50, 50, 50), size=(1280, 720), motion=0):
        bundle = args.cpu_bundle if backend == 2 else args.tensorrt_bundle
        r, t, d, s = controls
        path = out / (name + '.json')
        fixture = args.fixtures / ('face-4k.rgba' if size[0] == 3840 else 'face-720p.rgba')
        # Step assertions require an established endpoint before the first zero.
        # Record a common lead-in for CPU and GPU, separate from startup tests.
        steps = mode in (1, 4, 7)
        case_env = dict(env, BEAUTI_FIXTURE_CONTROL_START_SECONDS='4') if steps else env
        command = [
            str(args.harness.resolve()),
            str(bundle.resolve()),
            str(fixture.resolve()),
            str(path),
            '16' if steps else '12',
            '0',
            str(backend),
            str(r),
            str(s),
            str(size[0]),
            str(size[1]),
            str(motion),
            str(mode),
            '0',
            'en-US',
            'Beauti',
            str(t),
            str(d),
        ]
        memory = [vram()]
        with (out / (name + '.log')).open('w') as log:
            process = subprocess.Popen(command, stdout=log, stderr=log, env=case_env)
            start = time.monotonic()
            while process.poll() is None:
                if time.monotonic() - start > 90:
                    process.kill()
                    raise TimeoutError(name)
                time.sleep(1)
                memory.append(vram())
        assert process.returncode == 0, (name, process.returncode)
        log = (out / (name + '.log')).read_text()
        assert 'engine ready:' in log and f'STATUS: {size[0]}' in log, (name, log[-2000:])
        assert '[beauti] native' not in log and not re.search(r'\[beauti\] \S+ GPU: ', log), name
        data = json.loads(path.read_text())
        rows = np.asarray(data['samples'])
        rows = rows[rows[:, 0] >= 3]
        duration = float(rows[-1, 0] - rows[0, 0])
        processed = rows[rows[:, 3] > 0]
        assert len(processed) > 0, name + ': no processed frames were delivered'
        ages = (processed[:, 2] - processed[:, 1]) * 1000 / 30
        valid_memory = [x for x in memory if x is not None]
        records[name] = {
            'backend': backend,
            'controls': list(controls),
            'slider_mode': mode,
            'width': size[0],
            'height': size[1],
            'motion': bool(motion),
            'sample_seconds': duration,
            'output_fps': (len(rows) - 1) / duration,
            'distinct_processed_fps': max(0, len(np.unique(processed[:, 1])) - 1) / duration,
            'zero_difference_frames': int(np.count_nonzero(rows[:, 3] == 0)),
            'median_frame_age_ms': float(np.median(ages)),
            'p95_frame_age_ms': float(np.quantile(ages, 0.95)),
            'render_ms': data['render_ms'],
            'lagged_frames_after_warmup': data['lagged_frames_after_warmup'],
            'gpu_memory_baseline_mib': memory[0],
            'gpu_memory_peak_mib': max(valid_memory) if valid_memory else None,
            'gpu_memory_scope': 'Whole GPU usage sampled once per second, including other applications.',
        }
        (out / 'measurements.json').write_text(json.dumps(records, indent=2) + '\n')
        ui = json.loads(Path(str(path) + '.ui.json').read_text())
        assert [(item['name'], item['label']) for item in ui['properties'][:4]] == [
            ('retouch', 'Retouch'),
            ('skin_tone', 'Skin Tone'),
            ('dodge_burn', 'Micro Dodge & Burn'),
            ('smooth', 'Smooth'),
        ]
        print(name + ': ' + json.dumps(records[name]), flush=True)

    run('tensorrt-720p')
    run('tensorrt-fp32-720p', backend=4)
    run('cpu-720p', backend=2)
    run('tensorrt-drag', mode=5)
    run('tensorrt-steps', mode=1)
    run('cpu-steps', backend=2, mode=1)
    run('tone-drag', mode=3, controls=(0, 60, 0, 0))
    run('tone-steps', mode=4, controls=(0, 100, 0, 0))
    run('dodge-drag', mode=6, controls=(0, 0, 60, 0))
    run('dodge-steps', mode=7, controls=(0, 0, 100, 0))
    run('tensorrt-4k-motion', size=(3840, 2160), motion=1)
    checker = Path(__file__).with_name('check_slider_transitions.py')
    for prefix in ('tensorrt', 'tone', 'dodge'):
        command = [
            sys.executable,
            str(checker),
            '--drag',
            str(out / (prefix + '-drag.json')),
            '--steps',
            str(out / (prefix + '-steps.json')),
            '--output',
            str(out / (prefix + '-transitions.json')),
        ]
        if prefix == 'tensorrt':
            command += ['--cpu-steps', str(out / 'cpu-steps.json')]
        subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
