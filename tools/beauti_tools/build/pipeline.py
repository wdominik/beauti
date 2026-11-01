# SPDX-License-Identifier: GPL-3.0-or-later
"""Small, shared checks for the native build orchestrator."""

import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

from ..paths import ROOT

_TENSORRT = json.loads((ROOT / 'buildspec.json').read_text(encoding='utf-8'))['dependencies']['tensorrt-rtx']
NVIDIA_DLLS = frozenset(name for spec in _TENSORRT['files'].values() for name in spec['dlls'])


def default_paths(root: Path, system: str, cpu_only: bool) -> tuple[Path, Path]:
    if system == 'windows':
        device = 'cpu' if cpu_only else 'tensorrt'
        return root / 'build' / ('windows-' + device), root / 'dist' / device / 'beauti'
    return root / 'build/macos', root / 'dist/beauti.plugin'


def cmake_tools(command: str, system: str) -> tuple[str, str, str]:
    executable = shutil.which(str(command))
    if not executable:
        raise ValueError('CMake was not found: ' + str(command))
    cmake = Path(executable).resolve()
    ctest = cmake.with_name('ctest.exe' if system == 'windows' else 'ctest')
    if not ctest.is_file():
        raise ValueError('The selected CMake installation has no matching CTest: ' + str(ctest))
    versions = []
    for tool in (cmake, ctest):
        text = subprocess.check_output([str(tool), '--version'], text=True, timeout=30)
        match = re.search(r'version (\d+)\.(\d+)\.(\d+)', text)
        if not match:
            raise ValueError('Could not identify tool version: ' + str(tool))
        versions.append(tuple(map(int, match.groups())))
    minimum = (4, 2, 0) if system == 'windows' else (3, 25, 0)
    if versions[0] < minimum:
        raise ValueError('CMake ' + '.'.join(map(str, minimum)) + ' or newer is required')
    if versions[0] != versions[1]:
        raise ValueError('CMake and CTest versions differ in the selected installation')
    return str(cmake), str(ctest), '.'.join(map(str, versions[0]))


def validate_windows_inputs(sdk: Path | None, ort: Path | None) -> None:
    if sdk is None or ort is None:
        raise ValueError('--obs-sdk and --ort-root are required')
    required = [
        sdk / 'lib/obs.lib',
        sdk / 'lib/obs-frontend-api.lib',
        sdk / 'lib/cmake/libobs/libobsConfig.cmake',
        sdk / 'lib/cmake/obs-frontend-api/obs-frontend-apiConfig.cmake',
        ort / 'lib/onnxruntime.lib',
        ort / 'lib/onnxruntime.dll',
    ]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise ValueError('Missing native build inputs: ' + ', '.join(missing))


def build_environment(build: Path, version: str) -> dict[str, Any]:
    cache: dict[str, str] = {}
    for line in (build / 'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
        match = re.match(r'([^:#/][^:]*):[^=]+=(.*)', line)
        if match:
            cache[match[1]] = match[2]
    result: dict[str, Any] = {
        'cmake': version,
        'python_build_tool': sys.version.split()[0],
        'generator': cache.get('CMAKE_GENERATOR'),
        'configuration': 'Release',
    }
    compiler = build / 'CMakeFiles' / version / 'CMakeCXXCompiler.cmake'
    if compiler.is_file():
        text = compiler.read_text(encoding='utf-8')
        for name, key in [('CMAKE_CXX_COMPILER_ID', 'compiler'), ('CMAKE_CXX_COMPILER_VERSION', 'compiler_version')]:
            match = re.search(r'set\(' + name + r' "([^"]+)"\)', text)
            if match:
                result[key] = match[1]
    (build / 'build-environment.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def verification_binding(bundle: Path, report: dict[str, Any], device: str) -> dict[str, Any]:
    """A historical pass is not proof that a newly rebuilt artifact was tested."""
    # NVIDIA's runtime is not part of the package; test copies add it beside the plugin.
    expected = {
        name: sha for name, sha in report.get('artifact_sha256', {}).get(device, {}).items() if name not in NVIDIA_DLLS
    }
    if not expected:
        return {'status': 'historical', 'reason': 'Report has no runtime artifact hashes'}
    actual_files = {
        p.relative_to(bundle).as_posix() for p in bundle.iterdir() if p.is_file() and p.name not in NVIDIA_DLLS
    }
    actual_files |= {p.relative_to(bundle).as_posix() for p in (bundle / 'data/models').rglob('*') if p.is_file()}
    actual_files |= {'data/beauti.effect', 'data/continuity.effect', 'data/cache-key.txt'}
    actual_files |= {p.relative_to(bundle).as_posix() for p in (bundle / 'data/locale').glob('*.ini')}
    mismatches = sorted(actual_files ^ expected.keys())
    for name in sorted(actual_files & expected.keys()):
        with (bundle / name).open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != expected[name]:
                mismatches.append(name)
    return {
        'status': 'matched' if not mismatches else 'historical',
        'mismatches': mismatches,
        'scope': 'Runtime binary/resource identity only; does not establish unperformed camera or performance checks.',
    }
