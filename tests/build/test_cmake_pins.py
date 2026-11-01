# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real CMake reconfiguration when managed Windows runtime pins change."""

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from beauti_tools.build.build import configure_command

ROOT = Path(__file__).resolve().parents[2]


def tool(name):
    local = Path(sys.executable).with_name(name + ('.exe' if sys.platform == 'win32' else ''))
    return str(local) if local.is_file() else shutil.which(name) or name


def runtime(directory):
    directory.mkdir(parents=True)
    for name in ['onnxruntime.lib', 'onnxruntime.dll']:
        (directory / name).write_bytes(b'configuration fixture')


@pytest.mark.parametrize('device', ['cpu', 'tensorrt'])
@pytest.mark.parametrize('external', [False, True])
def test_pin_update_follows_managed_paths_and_preserves_external_paths(tmp_path, device, external):
    spec = json.loads((ROOT / 'buildspec.json').read_text(encoding='utf-8'))
    key = 'windows-x64'
    (tmp_path / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.25)\nproject(PinRegression NONE)\n'
        'set(BUILD_TESTING OFF)\nset(APPLE FALSE)\nset(WIN32 TRUE)\n'
        f'set(BEAUTI_ENABLE_TENSORRT {"ON" if device == "tensorrt" else "OFF"})\n'
        f'include("{(ROOT / "cmake/Dependencies.cmake").as_posix()}")\n',
        encoding='utf-8',
    )
    custom = tmp_path / 'custom runtime'
    runtime(custom)
    command = [
        tool('cmake'),
        '-S',
        str(tmp_path),
        '-B',
        str(tmp_path / 'out'),
        '-G',
        'Ninja',
        f'-DCMAKE_MAKE_PROGRAM={tool("ninja")}',
    ]
    overrides = (
        [
            f'-DBEAUTI_ORT_LIBRARY={custom.as_posix()}/onnxruntime.lib',
            f'-DBEAUTI_ORT_RUNTIME={custom.as_posix()}/onnxruntime.dll',
        ]
        if external
        else []
    )
    for version in ('old', 'new'):
        spec['dependencies']['onnxruntime'][key]['archive_root'] = version
        (tmp_path / 'buildspec.json').write_text(json.dumps(spec), encoding='utf-8')
        folder = tmp_path / '.cache/windows/ort_cpu' / version / 'lib'
        runtime(folder)
        subprocess.run(
            command + (overrides if version == 'old' else []), check=True, capture_output=True, text=True, timeout=30
        )
        cache = (tmp_path / 'out/CMakeCache.txt').read_text(encoding='utf-8')
        expected = custom if external else folder
        for variable, file in [('LIBRARY', 'onnxruntime.lib'), ('RUNTIME', 'onnxruntime.dll')]:
            assert f'BEAUTI_ORT_{variable}:FILEPATH={(expected / file).as_posix()}' in cache
        if version == 'old':
            for path in folder.iterdir():
                path.unlink()


@pytest.mark.parametrize(
    'system,cpu,preset', [('windows', True, 'windows-cpu'), ('windows', False, 'windows'), ('macos', False, 'macos')]
)
def test_orchestrator_uses_presets_with_explicit_overrides(tmp_path, system, cpu, preset):
    args = argparse.Namespace(
        cmake=tool('cmake'),
        cpu_only=cpu,
        obs_sdk=tmp_path / 'sdk',
        ort_root=tmp_path / 'ort',
        obs_app=tmp_path / 'OBS.app',
        ort_library=tmp_path / 'runtime.dylib',
    )
    command = configure_command(args, system, tmp_path / 'build')
    assert command[command.index('--preset') + 1] == preset
    assert command[command.index('-B') + 1] == str(tmp_path / 'build')
    assert '-G' not in command and '-A' not in command
    if system == 'windows':
        assert f'-DCMAKE_PREFIX_PATH={args.obs_sdk.resolve()}' in command
        assert f'-DBEAUTI_ORT_RUNTIME={args.ort_root.resolve()}/lib/onnxruntime.dll' in command
    else:
        assert f'-DBEAUTI_ORT_LIBRARY={args.ort_library.resolve()}' in command
