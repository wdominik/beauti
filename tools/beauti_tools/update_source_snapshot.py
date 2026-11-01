# SPDX-License-Identifier: GPL-3.0-or-later
"""Refresh the reviewed source identity snapshot."""

import hashlib
import json

from .paths import ROOT
from .source_files import source_files


def main():
    path = ROOT / 'docs/provenance.json'
    record = json.loads(path.read_text())
    record['source_snapshot'] = {
        'algorithm': 'sha256',
        'scope': 'Versionable source files except this self-referential register.',
        'files': {
            p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in source_files(ROOT)
            if p != path
        },
    }
    path.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8', newline='\n')
    print('Recorded', len(record['source_snapshot']['files']), 'source files.')


if __name__ == '__main__':
    main()
