# SPDX-License-Identifier: GPL-3.0-or-later
"""Package a staged native bundle without installing it into OBS."""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from ..build.staging import replacement_files
from ..paths import ROOT


def audit_snapshot(bundle: Path, mac: bool) -> dict:
    """Validate the exact copy that will enter the archive, without rewriting it."""
    try:
        result = subprocess.run(
            [
                sys.executable,
                '-m',
                'beauti_tools.packaging.' + ('audit_mac_bundle' if mac else 'audit_windows_bundle'),
                str(bundle),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError as error:
        raise ValueError('Bundle audit failed: ' + error.stderr.strip()) from error
    return json.loads(result.stdout)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('bundle', type=Path)
    p.add_argument('--output-dir', type=Path, default=ROOT / 'dist')
    a = p.parse_args()
    bundle = a.bundle.resolve()
    mac = (bundle / 'Contents/Resources/bundle.json').exists()
    res = bundle / ('Contents/Resources' if mac else 'data')
    manifest = json.loads((res / 'bundle.json').read_text(encoding='utf-8'))
    version = manifest['version']
    if mac and sys.platform != 'darwin':
        p.error('Package a signed Mac bundle on macOS')
    name = f'Beauti-{"Mac" if mac else "Windows"}-{version}-{"arm64" if mac else "x64"}'
    a.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='beauti-package-') as temp:
        folder = Path(temp) / name
        folder.mkdir()
        shutil.copytree(bundle, folder / bundle.name, symlinks=True, copy_function=shutil.copy)
        audit = audit_snapshot(folder / bundle.name, mac)
        destination = (
            '~/Library/Application Support/obs-studio/plugins/' if mac else '%ProgramData%\\obs-studio\\plugins\\'
        )
        text = (
            f'Beauti {version}\n\n'
            f'Quit OBS completely, then copy {bundle.name} into {destination}\n'
            '(replace an existing copy to update; delete it to uninstall).\n'
            "Add Beauti to your source's effect filters. The version is shown at the bottom\n"
            'of the settings. The full steps are in the included source/docs/INSTALL.md.\n\n'
            'The package contains models and native libraries. No Python runtime or\n'
            'external project folder is required.\n\n'
        )
        if mac:
            text += 'Apple silicon / macOS 27 or later / OBS Studio 33.0.0 or later.\n\n'
        else:
            text += 'Windows x64 / OBS Studio 33.0.0 or later (plugin folder layout of OBS 33).\n\n'
        if not mac and manifest.get('tensorrt_enabled'):
            text += (
                'This package runs on the CPU until you add NVIDIA acceleration. It needs a\n'
                'supported NVIDIA RTX GPU (Ampere or newer), a CUDA 13-capable driver, and four\n'
                "files from NVIDIA's release. See the included source/docs/INSTALL.md.\n\n"
            )
        verification = audit['integrity']['verification']
        status = verification['status']
        if status != 'missing':
            text += f'Verification artifact match (rechecked during packaging): {status}.\n'
            if status != 'matched':
                text += 'No matching runtime verification report establishes validation of this package.\n'
            text += 'Included report passed: ' + str(verification.get('reported_passed', 'not recorded')) + '.\n\n'
        text += (
            'License notices, corresponding source and build metadata are included with\n'
            'the bundle. See the source docs/BUILD.md and docs/TESTING.md for details.\n'
        )
        (folder / 'README.txt').write_text(text, encoding='utf-8')
        archive = a.output_dir / (name + '.zip')
        with replacement_files(archive, archive.with_suffix('.sha256')) as (staged, checksum):
            if mac:
                subprocess.run(['ditto', '-c', '-k', '--keepParent', str(folder), str(staged)], check=True)
            else:
                with zipfile.ZipFile(staged, 'w', zipfile.ZIP_DEFLATED) as z:
                    for file in folder.rglob('*'):
                        if file.is_file():
                            z.write(file, str(file.relative_to(folder.parent)))
            with zipfile.ZipFile(staged) as z:
                if z.testzip() is not None:
                    raise ValueError('Archive CRC verification failed')
            with staged.open('rb') as stream:
                sha = hashlib.file_digest(stream, 'sha256').hexdigest()
            checksum.write_text(sha + '  ' + archive.name + '\n', encoding='utf-8')
        print(json.dumps({'archive': str(archive), 'bytes': archive.stat().st_size, 'sha256': sha}))


if __name__ == '__main__':
    main()
