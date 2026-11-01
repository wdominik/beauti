# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch missing source weights and verify every downloaded byte by SHA-256."""

import argparse
import hashlib
import json
import shutil
import urllib.request

from ..paths import ROOT


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--check', action='store_true')
    a = p.parse_args()
    records = json.loads((ROOT / 'models/sources.json').read_text())['files']
    for name, spec in records.items():
        path = ROOT / name
        if not path.exists():
            if a.check or not name.startswith('.cache/models/'):
                raise SystemExit('Missing source: ' + name)
            path.parent.mkdir(parents=True, exist_ok=True)
            partial = path.with_suffix(path.suffix + '.partial')
            try:
                with urllib.request.urlopen(spec['source'], timeout=60) as incoming, partial.open('wb') as out:
                    shutil.copyfileobj(incoming, out)
                with partial.open('rb') as f:
                    actual = hashlib.file_digest(f, 'sha256').hexdigest()
                if actual != spec['sha256']:
                    raise RuntimeError('Downloaded source checksum mismatch: ' + name)
                partial.replace(path)
            finally:
                partial.unlink(missing_ok=True)
        with path.open('rb') as f:
            actual = hashlib.file_digest(f, 'sha256').hexdigest()
        if actual != spec['sha256']:
            raise SystemExit('Source checksum mismatch: ' + name)
        print('Verified ' + name)


if __name__ == '__main__':
    main()
