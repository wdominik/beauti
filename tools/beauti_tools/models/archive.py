# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack and unpack the prepared models for release builds.

Core AI conversion is not byte-reproducible, so release builds use the exact prepared
files instead of converting again. The archive is an asset of its own GitHub release,
whose tag follows from models/manifest.json: changed models need a new asset.
"""

import argparse
import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path

from ..paths import ROOT

MANIFEST = ROOT / 'models/manifest.json'
MODELS = ROOT / '.cache/models'
NOTICES = (
    'FaceMesh-Apache-2.0.txt',
    'Iris-Apache-2.0.txt',
    'MediaPipe-Apache-2.0.txt',
    'MediaPipe-NOTICE.txt',
    'ModelScope-Apache-2.0.txt',
    'ModelScope-NOTICE.txt',
)
ASSET = 'beauti-models.zip'


def release_tag(files: dict[str, str]) -> str:
    """The tag identifies the model set, independent of the manifest's formatting."""
    return 'models-' + hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()[:12]


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def pack(files: dict[str, str], models: Path, licenses: Path, output: Path) -> None:
    """Check every model first; the archive carries the notices its models require."""
    for name, expected in files.items():
        if digest(models / name) != expected:
            raise ValueError('Prepared model differs from the manifest: ' + name)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(files):
            archive.write(models / name, 'models/' + name)
        for name in NOTICES:
            archive.write(licenses / name, 'licenses/' + name)


def unpack(files: dict[str, str], archive_path: Path, models: Path) -> None:
    """Read only the names in the manifest; a member's own path is never used."""
    with zipfile.ZipFile(archive_path) as archive:
        for name, expected in files.items():
            target = models / name
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = target.with_name(target.name + '.partial')
            try:
                with archive.open('models/' + name) as source, partial.open('wb') as out:
                    shutil.copyfileobj(source, out)
            except KeyError as error:
                raise ValueError('Model archive lacks ' + name) from error
            if digest(partial) != expected:
                partial.unlink()
                raise ValueError('Archived model differs from the manifest: ' + name)
            partial.replace(target)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('tag', help='Print the GitHub release tag of the current model set')
    commands.add_parser('asset', help='Print the name of the archive in that release')
    commands.add_parser('pack', help='Write the archive from .cache/models').add_argument('output', type=Path)
    commands.add_parser('unpack', help='Check the archive into .cache/models').add_argument('archive', type=Path)
    args = parser.parse_args()
    files: dict[str, str] = json.loads(MANIFEST.read_text(encoding='utf-8'))['files']
    # No line ending: Python writes CRLF on Windows, which shell substitution would keep.
    if args.command == 'tag':
        sys.stdout.write(release_tag(files))
    elif args.command == 'asset':
        sys.stdout.write(ASSET)
    elif args.command == 'pack':
        pack(files, MODELS, ROOT / 'licenses', args.output)
    else:
        unpack(files, args.archive, MODELS)


if __name__ == '__main__':
    main()
