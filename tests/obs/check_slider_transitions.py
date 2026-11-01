# SPDX-License-Identifier: GPL-3.0-or-later
"""Assert actual output pixels keep their effect during strength changes.
Run obs_headless.cpp with mode 2 (continuous positive drag) and mode 1 (zero steps),
then pass reports here. This deliberately tests behavior, not internal ramp values.
"""

import argparse
import json
from pathlib import Path

import numpy as np


def read(path):
    d = json.loads(path.read_text())
    return d, np.array(d['samples'])


def run(drag, steps, output, old_drag=None, cpu_steps=None):
    """Analyze the drag and step reports; writes and returns the transition report."""
    report = {}
    if old_drag:
        _, s = read(old_drag)
        s = s[(s[:, 0] > 2.5) & (s[:, 0] < 7.9)]
        count = int(np.sum(s[:, 3] == 0))
        assert count > 0
        report['baseline_original_flashes_during_positive_drag'] = count
    d, s = read(drag)
    dragged = s[(s[:, 0] > 2.5) & (s[:, 0] < 7.9)]
    assert len(dragged) > 120 and np.all(dragged[:, 3] > 0)
    report['fixed_original_flashes_during_positive_drag'] = int(np.sum(dragged[:, 3] == 0))
    report['drag_frames_checked'] = len(dragged)
    report['lagged_render_frames'] = d['lagged_frames']
    for name, path in [('gpu', steps), ('cpu', cpu_steps)]:
        if not path:
            continue
        d, s = read(path)
        checks = []
        for base_second in [2, 4, 6, 8]:
            second = base_second + d.get('control_start_seconds', 0)
            before = s[(s[:, 0] > second - 0.5) & (s[:, 0] < second - 0.15), 3]
            after = s[(s[:, 0] > second + 0.45) & (s[:, 0] < second + 0.8), 3]
            assert len(before) > 3 and len(after) > 3
            lo, hi = sorted([float(np.median(before)), float(np.median(after))])
            assert hi - lo > 0.2
            transition = s[(s[:, 0] > second - 0.05) & (s[:, 0] < second + 0.4)]
            intermediate = int(
                np.sum((transition[:, 3] > lo + 0.05 * (hi - lo)) & (transition[:, 3] < hi - 0.05 * (hi - lo)))
            )
            assert intermediate >= 2, (name, second, transition)
            # More than one true intermediate frame, and no full endpoint jump.
            maximum_step = float(np.max(np.abs(np.diff(transition[:, 3]))))
            assert maximum_step < (hi - lo) * 0.8, (name, second, maximum_step, hi - lo)
            if base_second in [2, 6]:
                assert np.all(after == 0)
            else:
                assert np.all(after > 0)
            checks.append(
                {
                    'change_second': second,
                    'intermediate_frames': intermediate,
                    'maximum_step_fraction': maximum_step / (hi - lo),
                }
            )
        report[name + '_transitions'] = checks
    report['passed'] = True
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--old-drag', type=Path)
    p.add_argument('--drag', type=Path, required=True)
    p.add_argument('--steps', type=Path, required=True)
    p.add_argument('--cpu-steps', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    run(a.drag, a.steps, a.output, a.old_drag, a.cpu_steps)


if __name__ == '__main__':
    main()
