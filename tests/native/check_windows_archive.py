# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract a Windows ZIP, verify its models, and run face inference with a minimal PATH."""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import zipfile
from pathlib import Path


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('archive', 'fixture', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(args.archive) as archive:
        for entry in archive.infolist():
            if not (out / entry.filename).resolve().is_relative_to(out):
                raise ValueError('Unsafe archive member: ' + entry.filename)
        archive.extractall(out)  # zipfile checks each extracted member's CRC.
    bundles = list(out.glob('*/beauti'))
    assert len(bundles) == 1, bundles
    bundle = bundles[0]
    manifest = json.loads((bundle / 'data/bundle.json').read_text())
    models = manifest['model_sha256']
    assert len(models) == 8
    for name, expected in models.items():
        assert sha(bundle / 'data/models' / name) == expected, name
    env = os.environ.copy()
    env['PATH'] = str(Path(os.environ['SYSTEMROOT']) / 'System32')
    for name in ('PYTHONPATH', 'PYTHONHOME', 'CUDA_PATH', 'BEAUTI_ORT_PROFILE_DIR'):
        env.pop(name, None)
    backend = 'tensorrt' if manifest['tensorrt_enabled'] else 'cpu'
    worker = Path(__file__).with_name('check_worker.py').resolve()
    subprocess.run(
        [
            sys.executable,
            str(worker),
            '--bundle',
            str(bundle),
            '--fixture',
            str(args.fixture.resolve()),
            '--width',
            '1280',
            '--height',
            '720',
            '--backend',
            backend,
            '--output',
            str(out / 'worker'),
        ],
        env=env,
        cwd=out,
        check=True,
    )
    report = {
        'passed': True,
        'archive_sha256': sha(args.archive),
        'backend': backend,
        'model_hashes_verified': 8,
        'zip_crc_verified': True,
        'worker_cwd': str(out),
        'runtime_path': env['PATH'],
        'scope': 'The native executable uses only extracted bundle files and system/driver libraries. '
        'Python orchestrates this test.',
    }
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
