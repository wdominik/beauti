# SPDX-License-Identifier: GPL-3.0-or-later
"""Check eye-only slider modes 8/9, allowing one second for lazy analysis under TSan."""

import argparse
import json
from pathlib import Path

import numpy as np


def run(drag, steps, output):
    # The harness delays both schedules by its control lead-in (BEAUTI_FIXTURE_CONTROL_START_SECONDS).
    drag_report = json.loads(drag.read_text())
    rows = np.array(drag_report['samples'])
    lead = drag_report.get('control_start_seconds', 0)
    active = rows[(rows[:, 0] > lead + 2.5) & (rows[:, 0] < lead + 7.9)]
    assert len(active) > 120 and np.all(active[:, 3] > 0), 'Eye effect flashed off during positive dragging'
    steps_report = json.loads(steps.read_text())
    rows = np.array(steps_report['samples'])
    lead = steps_report.get('control_start_seconds', 0)
    transitions = []
    for second in (lead + 2, lead + 4, lead + 6, lead + 8):
        before = rows[(rows[:, 0] > second - 0.5) & (rows[:, 0] < second - 0.15), 3]
        after = rows[(rows[:, 0] > second + 1) & (rows[:, 0] < second + 1.5), 3]
        assert len(before) > 3 and len(after) > 3
        lo, hi = sorted([float(np.median(before)), float(np.median(after))])
        assert hi - lo > 0.001, 'Fixture did not exercise eye correction'
        transition = rows[(rows[:, 0] > second - 0.05) & (rows[:, 0] < second + 1), 3]
        intermediate = int(
            np.count_nonzero((transition > lo + 0.05 * (hi - lo)) & (transition < hi - 0.05 * (hi - lo)))
        )
        assert intermediate >= 2, 'Eye strength jumped to its endpoint'
        step = float(np.max(np.abs(np.diff(transition)))) / (hi - lo)
        assert step < 0.8
        if second - lead in (2, 6):
            off = rows[(rows[:, 0] > second + 0.4) & (rows[:, 0] < second + 1.5), 3]
            assert np.all(off == 0), 'Disabled eyes did not return exactly to source'
        else:
            assert np.all(after > 0), 'Eye analysis did not activate within one second'
        transitions.append({'second': second, 'intermediate_frames': intermediate, 'maximum_step_fraction': step})
    report = {
        'passed': True,
        'positive_drag_frames': len(active),
        'positive_drag_original_flashes': 0,
        'zero_returns_exact': True,
        'activation_deadline_seconds': 1,
        'transitions': transitions,
    }
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('drag', type=Path)
    parser.add_argument('steps', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    run(args.drag, args.steps, args.output)


if __name__ == '__main__':
    main()
