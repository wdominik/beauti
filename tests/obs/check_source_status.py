# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify source blockers and recovery in real OBS properties on Windows and macOS."""

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


def run(bundle, harness, fixture, output, obs_runtime, backend):
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    for key in ('BEAUTI_TIMING', 'BEAUTI_ORT_PROFILE_DIR', 'BEAUTI_FIXTURE_CUT', 'BEAUTI_CONTINUITY_TEST'):
        env.pop(key, None)
    env['BEAUTI_OBS_RUNTIME'] = str(obs_runtime.resolve())
    env['BEAUTI_FIXTURE_SOURCE_STATUS'] = '1'
    if sys.platform == 'win32':
        env['PATH'] = str(obs_runtime.resolve() / 'bin/64bit') + ';' + os.environ['SYSTEMROOT'] + '/System32'
    path = output / 'frames.json'
    command = [harness.resolve(), bundle.resolve(), fixture.resolve(), path.resolve(), 16, 0, backend]
    command += [60, 50, 1280, 720, 0, 0, 0, 'en-US', 'Beauti', 50, 50]
    with (output / 'obs.log').open('w', encoding='utf-8') as log:
        subprocess.run([str(c) for c in command], env=env, stdout=log, stderr=log, check=True, timeout=120)
    log = (output / 'obs.log').read_text(encoding='utf-8')
    assert '[beauti] native' not in log and 'engine ready:' in log, log[-3000:]
    states = json.loads(Path(str(path) + '.status.json').read_text(encoding='utf-8'))
    assert len(states) == 14, states
    expected = {
        'missing': 'Processing paused: no source is connected',
        'zero-width': 'Processing paused: the source has no image size yet',
        'zero-height': 'Processing paused: the source has no image size yet',
        'large-width': 'Processing paused: the source exceeds the size limit (4096 px)',
        'large-height': 'Processing paused: the source exceeds the size limit (4096 px)',
        **dict.fromkeys(
            ('source-16f', 'source-hdr', 'source-scrgb'), 'Processing paused: the source must use sRGB (SDR)'
        ),
        'canvas-16f': 'Processing paused: the canvas must use sRGB (SDR)',
    }
    for state in states:
        if state['case'] in expected:
            assert state['rendered'], state
            assert state['status'] == state['reopened'] == expected[state['case']], state
        elif state['case'] in ('before', 'recovered'):
            assert state['status'].startswith('1280 × 720'), state
        else:
            # OBS converts HDR/scRGB canvas rendering to sRGB around this source.
            # The filter must accept that supported context, not warn about HDR
            # that it never receives. A fresh analysis can finish before polling.
            assert state['case'] in ('canvas-hdr', 'canvas-scrgb', 'ready') and state['rendered'], state
            assert state['status'] == state['reopened'], state
            assert state['status'] == 'Starting…' or state['status'].startswith('1280 × 720'), state
    rows = json.loads(path.read_text())['samples']
    recovered = [r for r in rows if r[0] > 12]
    assert len(recovered) > 30 and all(r[3] > 0 for r in recovered), 'Processing did not recover'
    module = bundle / ('beauti.dll' if sys.platform == 'win32' else 'Contents/MacOS/beauti')
    report = {
        'passed': True,
        'backend': backend,
        'module_sha256': hashlib.sha256(module.read_bytes()).hexdigest(),
        'states': states,
        'recovered_processed_samples': len(recovered),
    }
    (output / 'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(f'Source status and recovery: backend {backend} passed', flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'harness', 'fixture', 'output', 'obs-runtime'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--backend', type=int, default=0 if sys.platform == 'darwin' else 4)
    args = parser.parse_args()
    run(args.bundle, args.harness, args.fixture, args.output, args.obs_runtime, args.backend)


if __name__ == '__main__':
    main()
