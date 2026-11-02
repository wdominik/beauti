# SPDX-License-Identifier: GPL-3.0-or-later
"""Face-tracking and flicker metrics over the per-frame results of a clip.

Consecutive maps are pasted into one coarse frame-sized grid through their face box, so
a moving box does not count as a change of the map. Works on model outputs, not on
composited pixels: GPU finishing stages are not part of these numbers.
"""

from dataclasses import dataclass, field

import cv2
import numpy as np

GRID_WIDTH = 320


def _percentiles(values):
    if not len(values):
        return {'median': None, 'p95': None, 'max': None}
    values = np.asarray(values, dtype=float)
    return {
        'median': float(np.median(values)),
        'p95': float(np.quantile(values, 0.95)),
        'max': float(values.max()),
    }


def _paste(values, box, scale, grid):
    """Resize a square map into its box on the coarse grid; outside the box stays zero."""
    left, top, right, bottom = (round(v * scale) for v in box)
    right = min(max(right, left + 1), grid[1])
    bottom = min(max(bottom, top + 1), grid[0])
    out = np.zeros(grid, dtype=np.float32)
    if left < grid[1] and top < grid[0]:
        out[top:bottom, left:right] = cv2.resize(values, (right - left, bottom - top), interpolation=cv2.INTER_LINEAR)
    return out


@dataclass
class ClipMetrics:
    """Feed every analyzed frame in order through add(), then read summary()."""

    width: int
    height: int
    states: list = field(default_factory=list)
    center_steps: list = field(default_factory=list)
    size_steps: list = field(default_factory=list)
    center_accel: list = field(default_factory=list)
    flips: list = field(default_factory=list)
    confidence_steps: list = field(default_factory=list)
    blend_steps: list = field(default_factory=list)
    input_steps: list = field(default_factory=list)
    totals: list = field(default_factory=list)
    previous: dict | None = None
    previous_step: tuple | None = None

    def add(self, status, box, mask, blend, pixels, total_ms):
        """mask and blend are square RGBA float maps, pixels the HxWx4 analysis frame."""
        self.totals.append(total_ms)
        hard = confidence = None
        if status == 0:
            hard, confidence = mask[..., 0], mask[..., 1]
        if status != 0:
            self.states.append('miss')
            self.previous = self.previous_step = None
            return
        self.states.append('empty' if not hard.any() else 'good')
        scale = GRID_WIDTH / self.width
        grid = (max(1, round(self.height * scale)), GRID_WIDTH)
        small = cv2.resize(pixels[..., :3], (grid[1], grid[0]), interpolation=cv2.INTER_AREA).astype(np.float32)
        current = {
            'box': box,
            'hard': _paste(hard, box, scale, grid) > 0.5,
            'confidence': _paste(confidence, box, scale, grid),
            'blend': np.stack([_paste(blend[..., c], box, scale, grid) for c in range(3)], axis=-1),
            'luma': small @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32),
        }
        if self.previous:
            self._compare(self.previous, current)
        else:
            self.previous_step = None
        self.previous = current

    def _compare(self, old, new):
        size = max(new['box'][2] - new['box'][0], new['box'][3] - new['box'][1], 1)
        step = np.array(
            [
                (new['box'][0] + new['box'][2] - old['box'][0] - old['box'][2]) / 2,
                (new['box'][1] + new['box'][3] - old['box'][1] - old['box'][3]) / 2,
            ]
        )
        self.center_steps.append(100 * float(np.hypot(*step)) / size)
        old_size = max(old['box'][2] - old['box'][0], old['box'][3] - old['box'][1], 1)
        self.size_steps.append(100 * abs(size - old_size) / size)
        if self.previous_step is not None:
            self.center_accel.append(100 * float(np.hypot(*(step - self.previous_step))) / size)
        self.previous_step = step
        union = old['hard'] | new['hard']
        if not union.any():
            return
        self.flips.append(100 * float(np.mean((old['hard'] ^ new['hard'])[union])))
        self.confidence_steps.append(float(np.mean(np.abs(old['confidence'] - new['confidence'])[union])))
        shared = old['hard'] & new['hard']
        if shared.any():
            self.blend_steps.append(float(np.mean(np.abs(old['blend'] - new['blend'])[shared])))
            self.input_steps.append(float(np.mean(np.abs(old['luma'] - new['luma'])[shared])))

    def summary(self):
        runs = _runs(self.states)
        misses = [length for state, length in runs if state == 'miss']
        empties = [length for state, length in runs if state == 'empty']
        seen_good = False
        losses = single = 0
        for state, length in runs:
            if state == 'miss' and seen_good:
                losses += length >= 2
                single += length == 1
            seen_good = seen_good or state == 'good'
        return {
            'frames': len(self.states),
            'good_frames': self.states.count('good'),
            'miss_frames': self.states.count('miss'),
            'empty_mask_frames': self.states.count('empty'),
            'first_good_frame': self.states.index('good') if 'good' in self.states else None,
            'single_misses': single,
            'losses_two_or_more_misses': losses,
            'longest_miss_run': max(misses, default=0),
            'empty_mask_runs': len(empties),
            'longest_empty_mask_run': max(empties, default=0),
            'center_step_percent_of_box': _percentiles(self.center_steps),
            'center_acceleration_percent_of_box': _percentiles(self.center_accel),
            'size_step_percent': _percentiles(self.size_steps),
            'mask_flip_percent': _percentiles(self.flips),
            'mask_confidence_step': _percentiles(self.confidence_steps),
            'blend_step': _percentiles(self.blend_steps),
            'input_luma_step': _percentiles(self.input_steps),
            'analysis_ms': _percentiles(self.totals),
        }


def _runs(states):
    runs = []
    for state in states:
        if runs and runs[-1][0] == state:
            runs[-1][1] += 1
        else:
            runs.append([state, 1])
    return runs
