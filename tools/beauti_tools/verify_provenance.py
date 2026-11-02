# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify source identities, optionally including prepared model files."""

import argparse
import hashlib
import json

from .paths import ROOT
from .source_files import snapshot_files


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--models', action='store_true')
    a = p.parse_args()
    provenance = json.loads((ROOT / 'docs/provenance.json').read_text())
    records = dict(provenance['source_snapshot']['files'])
    if a.models:
        records.update(
            {
                '.cache/models/' + name: sha
                for name, sha in json.loads((ROOT / 'models/manifest.json').read_text())['files'].items()
            }
        )
    failures: list[dict[str, str]] = []
    actual_names = {f.relative_to(ROOT).as_posix() for f in snapshot_files(ROOT)}
    expected_names = set(provenance['source_snapshot']['files'])
    failures.extend(
        {'path': name, 'reason': 'not in source snapshot'} for name in sorted(actual_names - expected_names)
    )
    failures.extend(
        {'path': name, 'reason': 'register path missing from ' + component['id']}
        for component in provenance['components']
        for name in component['local_paths']
        if not name.startswith('.cache/') and not (ROOT / name).exists()
    )
    if not expected_names:
        failures.append({'path': 'docs/provenance.json', 'reason': 'empty source snapshot'})
    for name, expected in sorted(records.items()):
        path = ROOT / name
        if not path.is_file():
            failures.append({'path': name, 'reason': 'missing'})
            continue
        with path.open('rb') as f:
            actual = hashlib.file_digest(f, 'sha256').hexdigest()
        if actual != expected:
            failures.append({'path': name, 'reason': 'SHA-256 mismatch'})
    print(
        json.dumps(
            {
                'version': provenance['version'],
                'checked_files': len(records),
                'passed': not failures,
                'failures': failures,
                'scope': 'File identity.',
            },
            indent=2,
        )
    )
    raise SystemExit(int(bool(failures)))


if __name__ == '__main__':
    main()
