# SPDX-License-Identifier: GPL-3.0-or-later
"""Check controlled TensorRT RTX dependency failures and CPU independence in a clean folder."""

import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path

_runtime = json.loads((Path(__file__).resolve().parents[2] / 'buildspec.json').read_text())['dependencies'][
    'tensorrt-rtx'
]['files']['tensorrt-rtx']['dlls']


def run(bundle, output):
    """Copy the worker into an empty folder and load runtime components step by step."""
    bundle = bundle.resolve()
    out = output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    worker = out / 'isolated-worker'
    worker.mkdir()
    for name in ('beauti-bench.exe', 'onnxruntime.dll'):
        shutil.copyfile(bundle / name, worker / name)
    env = os.environ.copy()
    env['PATH'] = str(Path(os.environ['SYSTEMROOT']) / 'System32')
    for key in list(env):
        if key.startswith('BEAUTI_'):
            env.pop(key)
    results = []

    def check(name, backend, expected_error=None):
        completed = subprocess.run(
            [
                str(worker / 'beauti-bench.exe'),
                '--models',
                str(bundle / 'data/models'),
                '--backend',
                backend,
                '--cache',
                str(out / 'cache'),
            ],
            input=b'',
            capture_output=True,
            env=env,
            cwd=worker,
            timeout=60,
            check=False,
        )
        message = completed.stderr.decode(errors='replace')
        assert completed.returncode == (1 if expected_error else 0), (name, completed.returncode, message)
        if expected_error:
            assert 'NVIDIA runtime file ' + expected_error in message, message
        results.append({'case': name, 'passed': True, 'exit_code': completed.returncode, 'message': message.strip()})

    check('CPU needs no NVIDIA libraries', 'cpu')
    check('Missing first component fails cleanly', 'tensorrt-fp32', _runtime[0])
    shutil.copyfile(bundle / _runtime[0], worker / _runtime[0])
    check('Partial loading unwinds cleanly', 'tensorrt-fp32', _runtime[1])
    report = {'passed': True, 'checks': results}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(results, indent=2))
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bundle', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    run(a.bundle, a.output)


if __name__ == '__main__':
    main()
