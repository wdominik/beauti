# SPDX-License-Identifier: GPL-3.0-or-later
"""pytest entry points for the libobs graphics checks.

The check_*.py modules next to this file keep their command lines for manual runs
and the Windows validation; the tests run the native runners from the build
directory with the shared fixtures from tests/conftest.py.
"""

import json
import os
import subprocess
import sys

import check_configurable_finishing
import check_presets
import cv2
import live_stats
import numpy as np
import pytest

import check_continuity
import check_finishing_pixels
import check_processing_defaults
import check_slider_transitions
import check_source_status

WINDOWS = sys.platform == 'win32'
DEFAULT_BACKEND = 4 if WINDOWS else 0

pytestmark = [pytest.mark.needs_bundle, pytest.mark.needs_gpu]


def runner_environment(obs_runtime):
    env = os.environ.copy()
    for key in ('BEAUTI_TIMING', 'BEAUTI_ORT_PROFILE_DIR', 'BEAUTI_FIXTURE_FPS', 'BEAUTI_FIXTURE_CUT'):
        env.pop(key, None)
    env['BEAUTI_OBS_RUNTIME'] = str(obs_runtime)
    if WINDOWS:
        env['PATH'] = str(obs_runtime / 'bin/64bit') + ';' + os.environ['SYSTEMROOT'] + '/System32'
    return env


def run_fixture(harness, bundle, fixture, obs_runtime, report, seconds, slider_mode, control_start=0):
    """One beauti-obs-check run at 1280x720, Performance quality, default controls.

    control_start delays the slider schedule, so a provider still starting is not
    measured as a broken transition (as in run_windows.py).
    """
    command = [harness, bundle, fixture, report, seconds, 0, DEFAULT_BACKEND, 60, 50, 1280, 720, 0, slider_mode, 0]
    command = [str(c) for c in command] + ['en-US', 'Beauti', '50', '50']
    env = runner_environment(obs_runtime)
    if control_start:
        env['BEAUTI_FIXTURE_CONTROL_START_SECONDS'] = str(control_start)
    with report.with_suffix('.log').open('w') as log:
        subprocess.run(command, env=env, stdout=log, stderr=log, check=True, timeout=120)
    return report


def test_live_statistics_describe_the_run(harness, bundle, fixture_720p, obs_runtime, tmp_path, monkeypatch):
    monkeypatch.setenv('BEAUTI_STATS', '1')
    report = run_fixture(harness, bundle, fixture_720p, obs_runtime, tmp_path / 'stats.json', 14, 0)
    filters = live_stats.parse(report.with_suffix('.log').read_text())
    assert len(filters) == 1
    stats = next(iter(filters.values()))
    assert stats['seconds'] > 9
    assert stats['processed'] > 0 and stats['accepted'] > 0
    assert stats['processed'] + stats['passthrough'] == stats['ticks']
    assert 0 < stats['age_p50'] <= stats['age_p95'] <= stats['age_max'] + 1
    assert stats['analysis_p50'] > 0
    assert stats['no_face'] == 0 and stats['bridged'] == 0


def test_automatic_rate_keeps_an_even_cadence(harness, bundle, fixture_720p, obs_runtime, tmp_path, monkeypatch):
    monkeypatch.setenv('BEAUTI_STATS', '1')
    env = runner_environment(obs_runtime)
    env['BEAUTI_FIXTURE_FPS'] = '0'
    env['BEAUTI_CANVAS_FPS'] = '60'
    report = tmp_path / 'automatic.json'
    command = [harness, bundle, fixture_720p, report, 14, 0, DEFAULT_BACKEND, 60, 50, 1280, 720, 0, 0]
    command = [str(c) for c in command] + ['en-US', 'Beauti', '50', '50']
    with report.with_suffix('.log').open('w') as log:
        subprocess.run(command, env=env, stdout=log, stderr=log, check=True, timeout=120)
    stats = next(iter(live_stats.parse(report.with_suffix('.log').read_text()).values()))
    # An even fraction of the canvas, up to 60 per second, with a low delay.
    assert 60 / stats['capture_fps'] == pytest.approx(round(60 / stats['capture_fps']), abs=0.01)
    assert stats['processed'] > 0
    assert stats['age_p50'] <= 2 * 1000 / stats['capture_fps'] + 1


def test_effects_at_zero_stop_the_analysis_and_release_the_models(
    continuity_bundle, harness, fixture_720p, obs_runtime, tmp_path, monkeypatch
):
    monkeypatch.setenv('BEAUTI_CONTINUITY_TEST', 'idle')
    report = run_fixture(harness, continuity_bundle, fixture_720p, obs_runtime, tmp_path / 'idle.json', 15, 14)
    log = report.with_suffix('.log').read_text()
    assert 'engine released' in log
    assert log.count('engine ready:') == 2
    rows = np.asarray(json.loads(report.read_text())['samples'])
    assert np.all(rows[(rows[:, 0] > 7) & (rows[:, 0] < 10), 3] == 0), 'The source must pass through untouched'
    assert np.any(rows[(rows[:, 0] > 2) & (rows[:, 0] < 4), 3] > 0), 'The effect must run before the pause'
    assert np.any(rows[rows[:, 0] > 12, 3] > 0), 'The effect must return after the pause'


def test_gpu_stages_match_the_cpu_reference(gpu_check, bundle, fixture_720p, obs_runtime, tmp_path):
    # The reference works on a crop inside the image, so the face must not reach its border.
    image = np.fromfile(fixture_720p, np.uint8).reshape(720, 1280, 4)
    matrix = np.array([[0.7, 0, 192], [0, 0.7, 108]], np.float32)
    fixture = tmp_path / 'inside.rgba'
    cv2.warpAffine(image, matrix, (1280, 720), flags=cv2.INTER_AREA, borderMode=cv2.BORDER_REPLICATE).tofile(fixture)
    out = tmp_path / 'pixels'
    out.mkdir()
    command = [str(gpu_check), str(bundle), str(fixture), str(out), '1280', '720']
    with (tmp_path / 'gpu-check.log').open('w') as log:
        subprocess.run(command, env=runner_environment(obs_runtime), stdout=log, stderr=log, check=True, timeout=900)
    left, top, right, bottom = json.loads((out / 'maps.json').read_text())['box']
    assert left >= 0 and top >= 0 and right <= 1280 and bottom <= 720, 'The face must stay inside the image here'
    report = check_finishing_pixels.run(out)
    assert report['passed']
    assert report['combinations'] == 81
    assert check_configurable_finishing.run(out)['passed']


def test_a_face_reaching_beyond_the_image_is_processed_up_to_the_border(
    gpu_check, bundle, fixture_720p, obs_runtime, tmp_path
):
    # Enlarged, so that the analysis region reaches beyond the image.
    image = np.fromfile(fixture_720p, np.uint8).reshape(720, 1280, 4)
    matrix = cv2.getRotationMatrix2D((640, 360), 0, 1.6)
    fixture = tmp_path / 'close.rgba'
    cv2.warpAffine(image, matrix, (1280, 720), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE).tofile(fixture)
    out = tmp_path / 'pixels'
    out.mkdir()
    command = [str(gpu_check), str(bundle), str(fixture), str(out), '1280', '720']
    with (tmp_path / 'gpu-check.log').open('w') as log:
        subprocess.run(command, env=runner_environment(obs_runtime), stdout=log, stderr=log, check=True, timeout=900)
    left, top, right, bottom = json.loads((out / 'maps.json').read_text())['box']
    assert right - left == bottom - top
    assert top < 0 or bottom > 720 or left < 0 or right > 1280
    before = np.fromfile(out / 'original.rgba', np.uint8).reshape(720, 1280, 4)
    after = np.fromfile(out / 'r100-t100-d100-s100.rgba', np.uint8).reshape(720, 1280, 4)
    assert np.mean(np.abs(after[..., :3].astype(int) - before[..., :3]) > 2) > 0.05


def test_presets_preserve_manual_values_and_hardware(harness, bundle, fixture_720p, obs_runtime, tmp_path):
    report = tmp_path / 'presets.json'
    command = [str(x) for x in [harness, bundle, fixture_720p, report, 3, 0, DEFAULT_BACKEND]]
    env = runner_environment(obs_runtime)
    env['BEAUTI_FIXTURE_PRESETS'] = '1'
    with report.with_suffix('.log').open('w') as log:
        subprocess.run(command, env=env, stdout=log, stderr=log, check=True, timeout=90)
    assert check_presets.run(report.with_name(report.name + '.presets.json'))['passed']


def test_fault_injection_keeps_the_output_continuous(continuity_bundle, harness, fixture_720p, obs_runtime, tmp_path):
    records = check_continuity.run(continuity_bundle, harness, fixture_720p, tmp_path / 'continuity', obs_runtime)
    assert set(records) == set(check_continuity.MODES)
    assert all(record['passed'] for record in records.values())


def test_strength_changes_keep_the_effect_on_screen(harness, bundle, fixture_720p, obs_runtime, tmp_path):
    drag = run_fixture(harness, bundle, fixture_720p, obs_runtime, tmp_path / 'drag.json', 10, 2)
    steps = run_fixture(harness, bundle, fixture_720p, obs_runtime, tmp_path / 'steps.json', 14, 1, control_start=4)
    report = check_slider_transitions.run(drag, steps, tmp_path / 'transitions.json')
    assert report['passed']
    assert report['fixed_original_flashes_during_positive_drag'] == 0


@pytest.mark.skipif(not WINDOWS, reason='Windows bundles only')
def test_processing_defaults_follow_the_build(cpu_bundle, bundle, harness, obs_runtime, fixture_720p, tmp_path):
    report = check_processing_defaults.run(
        cpu_bundle, bundle, harness, obs_runtime, fixture_720p, tmp_path / 'defaults'
    )
    assert report['passed']


@pytest.mark.parametrize('backend', [2, DEFAULT_BACKEND])
def test_source_blockers_are_visible_and_clear_on_recovery(
    harness, bundle, fixture_720p, obs_runtime, tmp_path, backend
):
    report = check_source_status.run(bundle, harness, fixture_720p, tmp_path / 'status', obs_runtime, backend)
    assert report['passed']
