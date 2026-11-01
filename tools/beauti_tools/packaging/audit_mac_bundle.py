# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify a relocatable macOS bundle has no external non-system native dependencies."""

import argparse
import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from .integrity import audit_payload

MAGIC = {b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('bundle', type=Path)
    args = parser.parse_args()
    # Verify an identical file snapshot outside cloud-synchronized folders.
    # Copy bytes/modes/symlinks, not Finder metadata; never re-sign the snapshot.
    with tempfile.TemporaryDirectory(prefix='beauti-audit-') as scratch:
        bundle = Path(scratch) / 'beauti.plugin'
        shutil.copytree(args.bundle.resolve(), bundle, symlinks=True, copy_function=shutil.copy)
        audit(bundle)


def audit(bundle):
    integrity = audit_payload(bundle, 'macos')
    binaries = []
    errors = []
    min_versions = []
    for p in bundle.rglob('*'):
        if p.is_symlink():
            if not p.resolve().is_relative_to(bundle):
                errors.append('External symlink: ' + str(p.relative_to(bundle)))
            continue
        if not p.is_file():
            continue
        with p.open('rb') as f:
            magic = f.read(4)
        if magic not in MAGIC:
            continue
        binaries.append(str(p.relative_to(bundle)))
        subprocess.run(['codesign', '--verify', '--strict', str(p)], check=True, capture_output=True)
        deps = subprocess.check_output(['otool', '-L', str(p)], text=True).splitlines()[1:]
        for line in deps:
            dep = line.strip().split(' (')[0]
            if not dep.startswith(('@rpath/', '@loader_path/', '@executable_path/', '/usr/lib/', '/System/Library/')):
                errors.append(str(p.relative_to(bundle)) + ': ' + dep)
        commands = subprocess.check_output(['otool', '-l', str(p)], text=True)
        errors.extend(
            'External rpath: ' + rpath
            for rpath in re.findall(r'cmd LC_RPATH\n.*?path (.*?) \(offset', commands, re.DOTALL)
            if not rpath.startswith(('@loader_path', '@executable_path'))
        )
        min_versions += re.findall(r'\bminos (\d+\.\d+(?:\.\d+)?)', commands)
    assert binaries, 'No Mach-O binaries'
    assert not errors, '\n'.join(errors)
    assert not (bundle / 'Contents/Resources/project-path.txt').exists()
    subprocess.run(['codesign', '--verify', '--strict', str(bundle)], check=True, capture_output=True)
    print(
        json.dumps(
            {
                'passed': True,
                'integrity': integrity,
                'mach_o_files_checked': len(binaries),
                'external_non_system_paths': errors,
                'external_symlinks': False,
                'maximum_binary_minimum_macos': max(min_versions, key=lambda v: tuple(map(int, v.split('.')))),
                'signed_bundle': True,
                'scope': 'Byte-identical file snapshot; filesystem metadata is excluded '
                'and signatures are not regenerated.',
            },
            indent=2,
        )
    )


if __name__ == '__main__':
    main()
