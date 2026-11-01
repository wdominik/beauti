# SPDX-License-Identifier: GPL-3.0-or-later
"""Check explicit native failures without changing the installed GPU driver."""

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


def run(cpu_bundle, tensorrt_bundle, output):
    """Provoke each failure in turn; writes and returns the report."""
    cpu, tensorrt = cpu_bundle.resolve(), tensorrt_bundle.resolve()
    out = output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.pop('BEAUTI_ORT_PROFILE_DIR', None)
    # Keep toolkit and development-runtime directories out of DLL search paths.
    env['PATH'] = str(Path(os.environ['SYSTEMROOT']) / 'System32')
    checks = {}

    def run(name, executable, models, backend, expected, environment=env):
        result = subprocess.run(
            [str(executable), '--models', str(models), '--backend', backend, '--cache', str(out / 'cache')],
            input=b'',
            capture_output=True,
            timeout=120,
            check=False,
            env=environment,
        )
        (out / (name + '.log')).write_bytes(result.stderr)
        assert result.returncode != 0, name + ' silently succeeded'
        assert not result.stdout, name + ' returned frames after initialization failed'
        assert expected.lower() in result.stderr.lower(), (name, result.stderr.decode(errors='replace'))
        checks[name] = {'passed': True, 'returncode': result.returncode}

    run(
        'tensorrt-unavailable-in-cpu-build',
        cpu / 'beauti-bench.exe',
        cpu / 'data/models',
        'tensorrt',
        b'not supported by this build',
    )
    run('missing-models', cpu / 'beauti-bench.exe', out / 'absent-models', 'cpu', b'Load model')
    with tempfile.TemporaryDirectory(prefix='missing-provider-', dir=out) as temporary:
        isolated = Path(temporary)
        # Omit only the provider; missing preloaded TensorRT RTX files would fail
        # earlier and test a different condition (check_windows_tensorrt_loading.py).
        for binary in tensorrt.iterdir():
            if binary.suffix.lower() in ('.exe', '.dll') and binary.name != 'onnxruntime_providers_nv_tensorrt_rtx.dll':
                shutil.copyfile(binary, isolated / binary.name)
        run(
            'missing-tensorrt-provider',
            isolated / 'beauti-bench.exe',
            tensorrt / 'data/models',
            'tensorrt',
            b'onnxruntime_providers_nv_tensorrt_rtx.dll',
        )
    hidden = dict(env, CUDA_VISIBLE_DEVICES='-1')
    run(
        'no-visible-tensorrt-device',
        tensorrt / 'beauti-bench.exe',
        tensorrt / 'data/models',
        'tensorrt',
        b'no compatible NVIDIA device',
        hidden,
    )
    report = {
        'passed': True,
        'checks': checks,
        'scope': 'Missing provider/model files, unavailable backend, and hidden GPU. '
        'The installed driver is unchanged.',
    }
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpu-bundle', type=Path, required=True)
    parser.add_argument('--tensorrt-bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    run(a.cpu_bundle, a.tensorrt_bundle, a.output)


if __name__ == '__main__':
    main()
