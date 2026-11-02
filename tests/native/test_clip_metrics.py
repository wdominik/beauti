# SPDX-License-Identifier: GPL-3.0-or-later
"""Contract of the clip metrics, and one clip run through a staged bundle."""

import sys

import clip_metrics
import numpy as np
import pytest
import run_clip

NATIVE_BACKEND = 'tensorrt' if sys.platform == 'win32' else 'coreai'
BOX = (100, 100, 300, 300)


def maps(hard=1.0, blend=0.0):
    mask = np.zeros((16, 16, 4), np.float32)
    mask[..., 0] = hard
    mask[..., 1] = hard
    return mask, np.full((16, 16, 4), blend, np.float32)


def frame(value=100):
    return np.full((360, 640, 4), value, np.uint8)


def test_identical_frames_have_no_flicker():
    metrics = clip_metrics.ClipMetrics(640, 360)
    for _ in range(3):
        metrics.add(0, BOX, *maps(), frame(), 10)
    summary = metrics.summary()
    assert summary['good_frames'] == 3
    assert summary['center_step_percent_of_box']['max'] == 0
    assert summary['mask_flip_percent']['max'] == 0
    assert summary['blend_step']['max'] == 0
    assert summary['input_luma_step']['max'] == 0


def test_box_step_is_relative_to_the_box():
    metrics = clip_metrics.ClipMetrics(640, 360)
    metrics.add(0, BOX, *maps(), frame(), 10)
    metrics.add(0, (110, 100, 310, 300), *maps(), frame(), 10)
    assert metrics.summary()['center_step_percent_of_box']['median'] == pytest.approx(5)


def test_a_moving_box_keeps_the_map_in_frame_coordinates():
    metrics = clip_metrics.ClipMetrics(640, 360)
    metrics.add(0, BOX, *maps(blend=0.5), frame(), 10)
    metrics.add(0, BOX, *maps(blend=0.75), frame(), 10)
    assert metrics.summary()['blend_step']['median'] == pytest.approx(0.25)


def test_misses_and_empty_masks_are_counted_as_runs():
    metrics = clip_metrics.ClipMetrics(640, 360)
    miss = (1, (0, 0, 0, 0), None, None, frame(), 5)
    good = (0, BOX, *maps(), frame(), 10)
    empty = (0, BOX, *maps(hard=0.0), frame(), 10)
    for item in (miss, good, miss, good, miss, miss, miss, good, empty, empty, good):
        metrics.add(*item)
    summary = metrics.summary()
    assert summary['first_good_frame'] == 1
    assert summary['single_misses'] == 1
    assert summary['losses_two_or_more_misses'] == 1
    assert summary['longest_miss_run'] == 3
    assert summary['empty_mask_runs'] == 1
    assert summary['longest_empty_mask_run'] == 2


def test_scenarios_repeat_exactly():
    image = np.full((720, 1280, 3), 120, np.uint8)
    for name in run_clip.SCENARIOS:
        first = next(run_clip.scenario_frames(image, name, (640, 360)))
        second = next(run_clip.scenario_frames(image, name, (640, 360)))
        assert first.shape == (720, 1280, 4)
        assert np.array_equal(first, second)


@pytest.mark.needs_bundle
def test_a_still_scenario_keeps_the_face(bundle, fixture_720p, tmp_path):
    image = run_clip.load_image(fixture_720p, 1280, 720)
    center = run_clip.face_center(image, bundle, NATIVE_BACKEND, tmp_path)
    frames = list(run_clip.scenario_frames(image, 'static', center))[:12]
    summary = run_clip.measure(frames, bundle, NATIVE_BACKEND, 'fast', 0, tmp_path)
    assert summary['frames'] == 12
    assert summary['good_frames'] == 12
    assert summary['center_step_percent_of_box']['median'] < 5
