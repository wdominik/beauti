# SPDX-License-Identifier: GPL-3.0-or-later
"""pytest entry points for the native engine checks.

The check_*.py modules next to this file keep their command lines for manual runs
and the Windows validation; the tests call the same functions with the shared
fixtures from tests/conftest.py and skip when a prerequisite is not configured.
"""

import sys

import pytest

import check_failure_modes
import check_worker
import compare_workers

WINDOWS = sys.platform == 'win32'
NATIVE_BACKEND = 'tensorrt' if WINDOWS else 'coreai'

pytestmark = pytest.mark.needs_bundle


@pytest.mark.parametrize('backend', ['cpu', NATIVE_BACKEND])
def test_worker_returns_maps_only_for_requested_effects(bundle, fixture_720p, tmp_path, backend):
    report = check_worker.run(bundle, fixture_720p, 1280, 720, backend, tmp_path / backend)
    assert report['passed']
    assert len(report['frames']) == len(check_worker.CONTROLS)


@pytest.mark.parametrize('backend', ['cpu', NATIVE_BACKEND])
def test_candidate_worker_matches_reference_bundle_exactly(reference_bundle, bundle, fixture_720p, tmp_path, backend):
    report = compare_workers.run(reference_bundle, bundle, fixture_720p, tmp_path / backend, backend, exact=True)
    assert report['passed']
    assert report['all_maps_exact']


@pytest.mark.skipif(not WINDOWS, reason='Windows bundles only')
def test_native_failures_are_explicit(cpu_bundle, bundle, tmp_path):
    report = check_failure_modes.run(cpu_bundle, bundle, tmp_path / 'failure-modes')
    assert report['passed']
