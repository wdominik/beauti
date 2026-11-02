# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the session summaries that BEAUTI_STATS=1 writes to an OBS log.

Each filter logs its cumulative counters every ten seconds and when it is removed, so
the last line per id is that filter's result.
"""

import argparse
import json
from pathlib import Path


def parse(text):
    """Return {filter id: {key: number}} from the last summary of each filter."""
    filters = {}
    for line in text.splitlines():
        _, marker, rest = line.partition('[beauti-stats] ')
        if not marker:
            continue
        fields = dict(item.split('=', 1) for item in rest.split()[1:])
        identity = fields.pop('id')
        filters[identity] = {key: float(value) for key, value in fields.items()}
    return filters


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    a = p.parse_args()
    print(json.dumps(parse(a.log.read_text(encoding='utf-8', errors='replace')), indent=2))


if __name__ == '__main__':
    main()
