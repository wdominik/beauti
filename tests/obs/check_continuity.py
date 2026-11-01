# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real presentation with the separate, non-installed fault-injection module."""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

MODES = ('miss', 'misses', 'delay', 'stall', 'error', 'cut')
DEFAULT_RUNTIME = Path('/Applications/OBS.app/Contents/Frameworks') if sys.platform == 'darwin' else None
DEFAULT_BACKEND = 0 if sys.platform == 'darwin' else 4


def run(
    bundle,
    harness,
    fixture,
    output,
    obs_runtime=DEFAULT_RUNTIME,
    backend=DEFAULT_BACKEND,
    quality=0,
    fps=30,
    modes=MODES,
    eyes=0,
):
    """Run every fault mode through the fixture runner; returns the per-mode records."""
    if obs_runtime is None:
        raise ValueError('obs_runtime is required on Windows')
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.pop('BEAUTI_TIMING', None)
    env.pop('BEAUTI_ORT_PROFILE_DIR', None)
    env['BEAUTI_OBS_RUNTIME'] = str(obs_runtime.resolve())
    env['BEAUTI_FIXTURE_FPS'] = str(fps)
    if sys.platform == 'win32':
        env['PATH'] = str(obs_runtime.resolve() / 'bin/64bit') + ';' + os.environ['SYSTEMROOT'] + '/System32'

    def command(out, seconds):
        return [
            str(harness.resolve()),
            str(bundle.resolve()),
            str(fixture.resolve()),
            str(out.resolve()),
            seconds,
            str(quality),
            str(backend),
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
            str(eyes),
        ]

    # A fresh bundle path has no compiled TensorRT RTX cache yet; a cold compile can outlast
    # the first fault, before any last-good frame exists. Warm it once without injection.
    env.pop('BEAUTI_CONTINUITY_TEST', None)
    env.pop('BEAUTI_FIXTURE_CUT', None)
    prewarm = output / 'prewarm.json'
    with prewarm.with_suffix('.log').open('w') as log:
        subprocess.run(command(prewarm, '8'), env=env, stdout=log, stderr=log, check=True, timeout=100)
    records = {}
    for mode in modes:
        env['BEAUTI_CONTINUITY_TEST'] = mode
        env.pop('BEAUTI_FIXTURE_CUT', None)
        if mode == 'cut':
            env['BEAUTI_FIXTURE_CUT'] = '1'
        out = output / (mode + '.json')
        with out.with_suffix('.log').open('w') as log:
            subprocess.run(command(out, '12'), env=env, stdout=log, stderr=log, check=True, timeout=100)
        log = out.with_suffix('.log').read_text()
        assert 'engine ready:' in log, log[-2000:]
        if mode != 'error':
            assert '[beauti] native' not in log, log[-2000:]
        else:
            assert 'Injected continuity test failure' in log
        rows = np.asarray(json.loads(out.read_text())['samples'])
        warm = rows[(rows[:, 0] >= 3) & (rows[:, 0] < 11)]
        if mode in ('miss', 'delay'):
            assert len(warm) > 200 and np.all(warm[:, 3] > 0), (mode, warm[warm[:, 3] == 0].tolist())
        if mode == 'miss':
            assert 'holding_no_face' in log and '[beauti-test] no-face 1' in log
        if mode == 'misses':
            assert 'confirmed_no_face' in log and '[beauti-test] no-face 2' in log
        if mode in ('stall', 'error'):
            assert 'reason=stall' in log and 'reason=recovering' in log
            assert np.count_nonzero(warm[:, 3] == 0) > 5, 'Stall did not reach bounded fallback'
        if mode == 'cut':
            cut = rows[(rows[:, 0] > 5.3) & (rows[:, 0] < 6.8)]
            assert len(cut) > 30 and np.all(cut[:, 4] > 0.99), 'Previous scene retained after a clear cut'
            assert np.all(rows[(rows[:, 0] > 9) & (rows[:, 0] < 11), 4] < 0.99), 'White scene persisted after return'
        assert np.all(rows[(rows[:, 0] > 9) & (rows[:, 0] < 11), 3] > 0), 'Failed to recover'
        records[mode] = {
            'passed': True,
            'samples': len(warm),
            'original_samples': int(np.count_nonzero(warm[:, 3] == 0)),
            'distinct_source_frames': len(np.unique(warm[:, 1])),
            'repeated_source_samples': int(np.count_nonzero(np.diff(warm[:, 1]) == 0)),
            'state_log': [line for line in log.splitlines() if '[beauti-continuity]' in line],
        }
        (output / 'report.json').write_text(json.dumps(records, indent=2) + '\n')
        print(mode, records[mode], flush=True)
    return records


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'harness', 'fixture', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--obs-runtime', type=Path, default=DEFAULT_RUNTIME)
    p.add_argument('--eyes', type=int, default=0)
    p.add_argument('--backend', type=int, choices=[0, 1, 2, 3, 4], default=DEFAULT_BACKEND)
    p.add_argument('--quality', type=int, choices=[0, 1], default=0)
    p.add_argument('--fps', type=int, choices=range(1, 61), default=30)
    p.add_argument('--modes', nargs='+', choices=MODES, default=list(MODES))
    a = p.parse_args()
    if a.obs_runtime is None:
        p.error('--obs-runtime is required on Windows')
    run(a.bundle, a.harness, a.fixture, a.output, a.obs_runtime, a.backend, a.quality, a.fps, a.modes, a.eyes)


if __name__ == '__main__':
    main()
