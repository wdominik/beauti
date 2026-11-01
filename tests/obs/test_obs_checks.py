# SPDX-License-Identifier: GPL-3.0-or-later
"""pytest entry points for the libobs graphics checks.

The check_*.py modules next to this file keep their command lines for manual runs
and the Windows validation; the tests run the native runners from the build
directory with the shared fixtures from tests/conftest.py.
"""

import os
import subprocess
import sys

import check_configurable_finishing
import check_presets
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


def test_gpu_stages_match_the_cpu_reference(gpu_check, bundle, fixture_720p, obs_runtime, tmp_path):
    out = tmp_path / 'pixels'
    out.mkdir()
    command = [str(gpu_check), str(bundle), str(fixture_720p), str(out), '1280', '720']
    with (tmp_path / 'gpu-check.log').open('w') as log:
        subprocess.run(command, env=runner_environment(obs_runtime), stdout=log, stderr=log, check=True, timeout=900)
    report = check_finishing_pixels.run(out)
    assert report['passed']
    assert report['combinations'] == 81
    assert check_configurable_finishing.run(out)['passed']


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
