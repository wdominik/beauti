# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarize opt-in host-wall timings; do not infer GPU kernel/copy durations."""

import argparse
import json
import re
import statistics
from collections import defaultdict
from pathlib import Path


def stats(values):
    values = sorted(values)
    if not values:
        return None
    pos = (len(values) - 1) * 0.95
    low = int(pos)
    p95 = values[low] + (values[min(low + 1, len(values) - 1)] - values[low]) * (pos - low)
    return {'count': len(values), 'median_ms': statistics.median(values), 'p95_ms': p95, 'maximum_ms': max(values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--profiles', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--min-frame', type=int, default=90)
    args = parser.parse_args()
    groups = defaultdict(list)
    for stage, frame, ms in re.findall(
        r'Beauti timing stage=(\w+) frame=(\d+) ms=([\d.]+)', args.log.read_text(encoding='utf-8', errors='replace')
    ):
        if int(frame) >= args.min_frame or int(frame) == 0:
            groups[stage].append(float(ms))
    assert groups, 'No timing events; set BEAUTI_TIMING=1 in the child process'
    report = {
        'scope': 'Host-wall spans, including CPU submission and synchronization. '
        'Overlapping stages must not be summed. These are not GPU event/kernel timings.',
        'minimum_frame_sequence': args.min_frame,
        'unsequenced_upload_scope': 'Upload spans have frame=0 and include startup; '
        'other stages exclude early frame sequences.',
        'stages': {name: stats(values) for name, values in sorted(groups.items())},
    }
    if args.profiles:
        profiles = {}
        for path in sorted(args.profiles.glob('*.json')):
            events = json.loads(path.read_text(encoding='utf-8'))
            profiles[path.name] = {
                name: stats([event['dur'] / 1000 for event in events if event.get('name') == name][10:])
                for name in ('model_run', 'SequentialExecutor::Execute')
            }
            profiles[path.name]['observed_node_threads'] = sorted(
                {str(e.get('tid')) for e in events if e.get('cat') == 'Node'}
            )
            profiles[path.name]['explicit_copy_node_events'] = sum(
                'memcpy' in e.get('name', '').lower() for e in events
            )
        report['ort_host_profiles'] = profiles
        report['ort_scope'] = (
            'First ten runs excluded. Model and executor times include host-side scheduling/synchronization; '
            'their difference is not a measurement of transfer time. Missing copy nodes do not imply zero transfers.'
        )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
