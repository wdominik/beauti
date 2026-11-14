# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the statistics that BEAUTI_STATS=1 writes to an OBS log.

Each filter logs its cumulative counters every ten seconds, when a setting it measures
changes, and when it is removed. A change of the source, the canvas, the device, the
quality, the analysis resolution, the target rate, or of Retouch or Eyes between zero and
nonzero starts a new segment, so one session can measure several configurations. The
last line of a segment is its result.
"""

import argparse
import json
from pathlib import Path

DEVICES = {0: 'Core AI', 2: 'CPU', 3: 'TensorRT', 4: 'TensorRT full precision'}
QUALITIES = {0: 'Performance', 1: 'High Quality'}


def segments(text):
    """Return the last summary of every segment in log order, as {key: number} with the filter id.

    Logs written before segments existed hold one segment per filter.
    """
    found = {}
    for line in text.splitlines():
        _, marker, rest = line.partition('[beauti-stats] ')
        if not marker:
            continue
        fields = dict(item.split('=', 1) for item in rest.split()[1:])
        identity = fields.pop('id')
        values = {key: float(value) for key, value in fields.items()}
        segment = int(values.setdefault('segment', 1))
        # A later summary of the same segment replaces the earlier one in place.
        found[identity, segment] = {'id': identity, **values}
    return list(found.values())


def parse(text):
    """Return {filter id: {key: number}} from the last summary of each filter."""
    return {row['id']: {key: value for key, value in row.items() if key != 'id'} for row in segments(text)}


def _cell(row, text, *keys):
    """Format the keys of a row, or a dash when a log from an older version lacks one."""
    return text.format(*(row[key] for key in keys)) if all(key in row for key in keys) else '-'


def table(rows, min_seconds=5.0):
    """A Markdown table with one line per segment that lasted at least min_seconds."""
    columns = (
        'Filter',
        'Segment',
        'Source',
        'Analysis',
        'Canvas FPS',
        'Device',
        'Quality',
        'Target FPS',
        'Retouch/Eyes',
        'Seconds',
        'New FPS',
        'Capture FPS',
        'Rate changes',
        'Age p50/p95 ms',
        'Analysis p50/p95 ms',
        'Compose p50 ms',
        'Readback p95/max ms',
        'OBS frame ms',
        'Lagged',
        'Losses',
        'Startup ms',
        'Recovery max ms',
    )
    lines = ['| ' + ' | '.join(columns) + ' |', '|' + ' --- |' * len(columns)]
    names = {}
    for row in rows:
        if row.get('seconds', 0) < min_seconds:
            continue
        device = DEVICES.get(int(row['backend']), str(int(row['backend']))) if 'backend' in row else '-'
        quality = QUALITIES.get(int(row['quality']), str(int(row['quality']))) if 'quality' in row else '-'
        target = (
            '-' if 'target_fps' not in row else 'Automatic' if not row['target_fps'] else f'{row["target_fps"]:.0f}'
        )
        seconds = row.get('seconds', 0)
        cells = (
            str(names.setdefault(row['id'], len(names) + 1)),
            f'{row["segment"]:.0f}',
            _cell(row, '{:.0f}x{:.0f}', 'source_w', 'source_h'),
            _cell(row, '{:.0f}x{:.0f}', 'analysis_w', 'analysis_h'),
            _cell(row, '{:g}', 'canvas_fps'),
            device,
            quality,
            target,
            _cell(row, '{:.0f}/{:.0f}', 'retouch', 'eyes'),
            f'{seconds:.0f}',
            f'{row["accepted"] / seconds:.1f}' if seconds and 'accepted' in row else '-',
            _cell(row, '{:.1f}', 'capture_fps'),
            _cell(row, '{:.0f}', 'rate_changes'),
            _cell(row, '{:.0f}/{:.0f}', 'age_p50', 'age_p95'),
            _cell(row, '{:.0f}/{:.0f}', 'analysis_p50', 'analysis_p95'),
            _cell(row, '{:.0f}', 'compose_p50'),
            _cell(row, '{:.0f}/{:.0f}', 'readback_p95', 'readback_max'),
            _cell(row, '{:.1f}', 'obs_frame_ms'),
            _cell(row, '{:.0f}', 'lagged'),
            _cell(row, '{:.0f}', 'losses'),
            _cell(row, '{:.0f}', 'startup_ms'),
            _cell(row, '{:.0f}', 'recovery_max'),
        )
        lines.append('| ' + ' | '.join(cells) + ' |')
    return '\n'.join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('--table', action='store_true', help='Print a Markdown table instead of JSON')
    p.add_argument('--min-seconds', type=float, default=5, help='Leave shorter segments out of the table')
    a = p.parse_args()
    rows = segments(a.log.read_text(encoding='utf-8', errors='replace'))
    print(table(rows, a.min_seconds) if a.table else json.dumps(rows, indent=2))


if __name__ == '__main__':
    main()
