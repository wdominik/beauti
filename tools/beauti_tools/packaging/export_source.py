# SPDX-License-Identifier: GPL-3.0-or-later
"""Export the repository source, optionally with the verified runtime models."""

import argparse
import hashlib
import json
import zipfile
from pathlib import Path

from ..paths import ROOT
from ..source_files import source_files


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--include-models', action='store_true')
    a = p.parse_args()
    version = (ROOT / 'VERSION').read_text().strip()
    prefix = f'Beauti-{version}-Source'
    files = [(f, str(f.relative_to(ROOT))) for f in source_files(ROOT)]
    if a.include_models:
        for name, expected in json.loads((ROOT / 'models/manifest.json').read_text())['files'].items():
            file = ROOT / '.cache/models' / name
            with file.open('rb') as stream:
                actual = hashlib.file_digest(stream, 'sha256').hexdigest()
            if actual != expected:
                raise RuntimeError('Model checksum mismatch: ' + name)
            files.append((file, '.cache/models/' + name))
    a.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(a.output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for file, name in sorted(files, key=lambda item: item[1]):
            archive.write(file, prefix + '/' + name)
    print(
        json.dumps(
            {
                'file': str(a.output),
                'files': len(files),
                'version': version,
                'runtime_models_included': a.include_models,
            }
        )
    )


if __name__ == '__main__':
    main()
