# SPDX-License-Identifier: GPL-3.0-or-later
"""Every locale catalog carries the same keys as en-US.ini, in OBS ini syntax.

OBS falls back to en-US for a missing key, so a gap does not break the plugin;
it hides a string from one language. The check keeps the catalogs complete and
well formed (one Key="Value" per line, UTF-8, no duplicates). Run it from the
repository root; the pre-commit hook and tests/locale/test_locales.py call run().
"""

import argparse
import re
import sys
from pathlib import Path

REFERENCE = 'en-US.ini'
LINE = re.compile(r'^([A-Za-z][A-Za-z0-9]*)="(.*)"$')


def parse(path):
    """Return the key/value pairs of one catalog; raise ValueError on bad syntax."""
    values = {}
    for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip():
            continue
        match = LINE.match(line)
        if not match:
            raise ValueError(f'{path.name}:{number}: expected Key="Value", got {line!r}')
        key, value = match.groups()
        if key in values:
            raise ValueError(f'{path.name}:{number}: duplicate key {key}')
        if not value.strip():
            raise ValueError(f'{path.name}:{number}: empty value for {key}')
        values[key] = value
    return values


def run(directory):
    """Compare every catalog in the directory with the reference; return the report."""
    directory = Path(directory)
    reference = parse(directory / REFERENCE)
    report = {'reference': REFERENCE, 'keys': len(reference), 'catalogs': {}, 'problems': []}
    for path in sorted(directory.glob('*.ini')):
        if path.name == REFERENCE:
            continue
        try:
            values = parse(path)
        except ValueError as error:
            report['problems'].append(str(error))
            continue
        missing = sorted(set(reference) - set(values))
        extra = sorted(set(values) - set(reference))
        report['catalogs'][path.name] = {'missing': missing, 'extra': extra}
        for key in missing:
            report['problems'].append(f'{path.name}: missing {key}')
        for key in extra:
            report['problems'].append(f'{path.name}: unknown key {key}')
    report['passed'] = not report['problems']
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--directory', default=Path(__file__).resolve().parents[2] / 'data/locale')
    args = parser.parse_args()
    report = run(args.directory)
    for problem in report['problems']:
        print(problem, file=sys.stderr)
    print(f'{len(report["catalogs"]) + 1} catalogs, {report["keys"]} keys: {"ok" if report["passed"] else "FAILED"}')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
