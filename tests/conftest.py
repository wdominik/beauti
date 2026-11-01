# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared fixtures for the Python tests.

Checks that need build output or a face fixture are opt-in through the
environment; a missing prerequisite skips the test and names the variable:

    BEAUTI_BUNDLE            staged plugin bundle (default: dist/beauti.plugin on
                             macOS, dist/tensorrt/beauti on Windows)
    BEAUTI_CPU_BUNDLE        Windows CPU bundle (default: dist/cpu/beauti)
    BEAUTI_REFERENCE_BUNDLE  previous bundle for the A/B worker comparison
    BEAUTI_FIXTURE_720P      tightly packed 1280x720 RGBA8 frame showing a face
    BEAUTI_BUILD_DIR         native build directory with the runners (default:
                             build/macos or build/windows-tensorrt/Release)
    BEAUTI_OBS_RUNTIME       OBS runtime directory (default on macOS: the installed app)

Markers (declared in pyproject.toml): needs_reference, needs_bundle, needs_gpu.
`pytest -m "not needs_bundle and not needs_gpu"` is what CI runs.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
WINDOWS = sys.platform == 'win32'
DARWIN = sys.platform == 'darwin'
REFERENCE_LIBRARY = (
    ROOT
    / 'build/effects/reference'
    / ('beauti-effects.dll' if WINDOWS else 'libbeauti-effects.dylib' if DARWIN else 'libbeauti-effects.so')
)


def pytest_collection_modifyitems(items):
    if REFERENCE_LIBRARY.is_file():
        return
    skip = pytest.mark.skip(reason='reference library missing: uv run beauti-build-reference')
    for item in items:
        if item.get_closest_marker('needs_reference'):
            item.add_marker(skip)


def _configured(variable, default, what):
    value = os.environ.get(variable)
    path = Path(value).resolve() if value else default
    if path is None or not path.exists():
        pytest.skip(f'{what} not available; set {variable}')
    return path


@pytest.fixture(scope='session')
def bundle():
    return _configured(
        'BEAUTI_BUNDLE', ROOT / ('dist/tensorrt/beauti' if WINDOWS else 'dist/beauti.plugin'), 'plugin bundle'
    )


@pytest.fixture(scope='session')
def cpu_bundle():
    return _configured('BEAUTI_CPU_BUNDLE', ROOT / 'dist/cpu/beauti' if WINDOWS else None, 'Windows CPU bundle')


@pytest.fixture(scope='session')
def reference_bundle():
    return _configured('BEAUTI_REFERENCE_BUNDLE', None, 'reference bundle for the A/B comparison')


@pytest.fixture(scope='session')
def fixture_720p():
    path = _configured('BEAUTI_FIXTURE_720P', None, '1280x720 RGBA fixture')
    if path.stat().st_size != 1280 * 720 * 4:
        pytest.fail(f'{path} is not a tightly packed 1280x720 RGBA8 frame')
    return path


@pytest.fixture(scope='session')
def build_dir():
    default = ROOT / ('build/windows-tensorrt/Release' if WINDOWS else 'build/macos')
    return _configured('BEAUTI_BUILD_DIR', default, 'native build directory')


def _runner(build_dir, name):
    path = build_dir / (name + '.exe' if WINDOWS else name)
    if not path.is_file():
        pytest.skip(f'{path.name} missing in {build_dir}; build the native tree first')
    return path


@pytest.fixture(scope='session')
def gpu_check(build_dir):
    return _runner(build_dir, 'beauti-gpu-check')


@pytest.fixture(scope='session')
def harness(build_dir):
    return _runner(build_dir, 'beauti-obs-check')


@pytest.fixture(scope='session')
def obs_runtime():
    default = Path('/Applications/OBS.app/Contents/Frameworks') if DARWIN else None
    return _configured('BEAUTI_OBS_RUNTIME', default, 'OBS runtime directory')


@pytest.fixture(scope='session')
def continuity_bundle(bundle, build_dir, tmp_path_factory):
    """An isolated copy of the bundle with the fault-injection module in place of the plugin."""
    module = build_dir / ('beauti-continuity-test.dll' if WINDOWS else 'beauti-continuity-test.so')
    if not module.is_file():
        pytest.skip(f'{module.name} missing in {build_dir}; build with BUILD_TESTING=ON')
    copy = tmp_path_factory.mktemp('continuity') / bundle.name
    if DARWIN:
        subprocess.run(['ditto', '--norsrc', '--noextattr', str(bundle), str(copy)], check=True)
        shutil.copyfile(module, copy / 'Contents/MacOS/beauti')
        subprocess.run(['xattr', '-cr', str(copy)], check=True)
        subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(copy)], check=True, capture_output=True)
    else:
        shutil.copytree(bundle, copy)
        shutil.copyfile(module, copy / 'beauti.dll')
    return copy
