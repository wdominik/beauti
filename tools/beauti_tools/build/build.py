# SPDX-License-Identifier: GPL-3.0-or-later
"""Configure, build, test, and stage Beauti with the shared CMake project.
Python is a build tool only. This does not install into OBS or launch its UI.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

from ..packaging.integrity import audit_payload, platform_models, seal_payload
from ..packaging.package import audit_snapshot
from ..paths import ROOT
from ..source_files import source_files
from .pipeline import build_environment, cmake_tools, default_paths, validate_windows_inputs, verification_binding
from .staging import replacement_directory

VERSION = (ROOT / 'VERSION').read_text().strip()
MODELS = ROOT / '.cache/models'
LICENSES = ROOT / 'licenses'
BUILDSPEC = json.loads((ROOT / 'buildspec.json').read_text(encoding='utf-8'))['dependencies']


def run(*args: object) -> None:
    subprocess.run([str(x) for x in args], check=True)


def cmake_cache(build: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in (build / 'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
        name, _, value = line.partition('=')
        if ':' in name and not name.startswith(('#', '//')):
            values[name.split(':', 1)[0]] = value
    return values


def digest(path: Path) -> str:
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


# ---------------------------------------------------------------------------
# Staging: models, notices, source, manifest, signature


def copy_models(res: Path, system: str) -> dict[str, str]:
    """Copy this platform's prepared models after checking them against the manifest."""
    manifest: dict[str, str] = json.loads((ROOT / 'models/manifest.json').read_text())['files']
    expected = platform_models(manifest, system)
    for name, sha in expected.items():
        source = MODELS / name
        if not source.is_file() or digest(source) != sha:
            raise RuntimeError('Missing or modified model: ' + name)
        target = res / 'models' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    return expected


def copy_notices(res: Path, system: str, runtime: Path, sdk: Path) -> None:
    """License texts: project, OBS, SIMDe, and (Windows) the ONNX Runtime notices."""
    shutil.copyfile(ROOT / 'COPYING', res / 'COPYING')
    shutil.copytree(LICENSES, res / 'LICENSES', copy_function=shutil.copyfile)
    if system == 'macos':
        shutil.copyfile(sdk / 'COPYING', res / 'LICENSES/OBS-Studio-COPYING.txt')
        shutil.copyfile(sdk.parent / 'simde/COPYING', res / 'LICENSES/SIMDe-MIT.txt')
        return
    shutil.copyfile(sdk / 'COPYING', res / 'LICENSES/OBS-Studio-COPYING.txt')
    shutil.copyfile(sdk / 'SIMDe-MIT.txt', res / 'LICENSES/SIMDe-MIT.txt')
    runtime_license = res / 'LICENSES/ONNX-Runtime-Windows'
    runtime_license.mkdir()
    for file in runtime.parents[1].iterdir():
        if file.is_file() and any(x in file.name.lower() for x in ['license', 'notice']):
            shutil.copyfile(file, runtime_license / file.name)
    if not any(runtime_license.iterdir()):
        raise RuntimeError('ONNX Runtime license notices are missing')


def copy_source(res: Path) -> Path:
    """The corresponding source accompanies every bundle (GPL-3.0-or-later)."""
    source = res / 'source'
    source.mkdir()
    for file in source_files(ROOT):
        destination = source / file.relative_to(ROOT)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(file, destination)
    return source


def write_manifest(
    bundle: Path,
    res: Path,
    source: Path,
    system: str,
    obs_version: str,
    tensorrt_enabled: bool,
    models: dict[str, str],
    runtime_hash: str,
    cache_key: str,
    environment: dict[str, Any] | None,
) -> None:
    """bundle.json records identities and binds the verification report to artifact hashes."""
    manifest: dict[str, Any] = {
        'product': 'Beauti',
        'version': VERSION,
        'platform': system,
        'architecture': 'arm64' if system == 'macos' else 'x64',
        'obs_build': obs_version,
        'python_runtime': False,
        'tensorrt_enabled': tensorrt_enabled,
        'model_sha256': models,
        'runtime_sha256': runtime_hash,
        'cache_key': cache_key,
        'source_id': 'beauti_filter',
        'strength_transition_ms': 150,
        'windows_validation': (
            'See verification.json and source/docs/TESTING.md for measured fixture results '
            'and unverified live-camera/encoding limits'
        )
        if system == 'windows'
        else 'Not applicable',
        'pipeline': {
            'order': ['retouch', 'skin_tone', 'local_color', 'dodge_burn', 'smooth', 'eyes'],
            'defaults': [70, 70, 30, 55, 40, 50],
            'default_preset': {'id': 102, 'name': 'Medium', 'catalog_version': 2},
            'local_color': 'Full-resolution masked Lab chroma smoothing with a fixed guide; zero bypasses',
            'detail_defaults': {
                'smooth_fine': 35,
                'smooth_small': 90,
                'smooth_medium': 80,
                'smooth_broad': 25,
                'smooth_size': 115,
                'smooth_edges': 110,
                'local_color_size': 110,
                'local_color_edges': 100,
            },
            'eyes': 'CPU FaceMesh V2 lids + Iris refinement; GPU iris/sclera exposure',
            'smooth': 'Texture: full-resolution, four-scale, range-weighted L* contrast reduction',
            'dodge_burn': 'Full-resolution masked local L* correction',
            'finishing_grid': 'Full-resolution face ROI',
            'skin_tone': 'GPU D65 Lab chroma equalization',
            'skin_tone_field_long_side': [256, 384],
            'skin_tone_median_samples': [64, 64],
            'skin_tone_histogram_bin_width': 0.25,
            'strengths_on_gpu': True,
        },
        'effect_sha256': digest(ROOT / 'data/beauti.effect'),
        'provenance_sha256': digest(ROOT / 'docs/provenance.json'),
        'build_environment': environment or {},
    }
    report_suffix = '-windows-tensorrt' if system == 'windows' else ''
    verification = ROOT / 'tests/reports' / f'beauti-{VERSION}{report_suffix}.json'
    if verification.exists():
        shutil.copyfile(verification, res / 'verification.json')
        (source / 'tests/reports').mkdir(parents=True, exist_ok=True)
        shutil.copyfile(verification, source / 'tests/reports' / verification.name)
        if system == 'windows':
            manifest['verification'] = verification_binding(
                bundle, json.loads(verification.read_text()), 'tensorrt' if tensorrt_enabled else 'cpu'
            )
            if manifest['verification']['status'] != 'matched':
                print(
                    'WARNING: The included verification report is historical; '
                    'these runtime artifacts require validation.',
                    flush=True,
                )
    elif system == 'windows':
        manifest['verification'] = {'status': 'missing'}
    (res / 'bundle.json').write_text(json.dumps(manifest, indent=2) + '\n')


def sign_bundle(bundle: Path, res: Path) -> None:
    """Ad-hoc signature for local use; release signing is a separate step (docs/RELEASE.md)."""

    def clear_finder_metadata() -> None:
        # Finder may refresh bundle attributes while nested binaries are signed.
        for name in ('com.apple.FinderInfo', 'com.apple.ResourceFork'):
            result = subprocess.run(
                ['xattr', '-r', '-d', name, str(bundle)], capture_output=True, text=True, check=False
            )
            if result.returncode and 'No such xattr' not in result.stderr:
                raise RuntimeError(result.stderr)

    frameworks = bundle / 'Contents/Frameworks'
    for target in [
        frameworks / 'libbeauti_onnxruntime.dylib',
        frameworks / 'libbeauti_coreai.dylib',
        res / 'bin/beauti-bench',
        bundle,
    ]:
        clear_finder_metadata()
        run('codesign', '--force', '--sign', '-', target)
    clear_finder_metadata()
    run('codesign', '--verify', '--strict', bundle)


def assemble(
    stage: Path,
    system: str,
    runtime: Path,
    obs_version: str,
    sdk: Path,
    tensorrt_enabled: bool = False,
    environment: dict[str, Any] | None = None,
) -> Path:
    """Turn the CMake install tree into a self-contained bundle."""
    bundle = stage / ('beauti.plugin' if system == 'macos' else 'beauti')
    res = bundle / ('Contents/Resources' if system == 'macos' else 'data')
    res.mkdir(parents=True, exist_ok=True)
    models = copy_models(res, system)
    runtime_hash = digest(runtime)
    cache_inputs: list[Any] = [models, runtime_hash]
    if tensorrt_enabled:
        cache_inputs.append(tensorrt_pins())
    cache_key = hashlib.sha256(json.dumps(cache_inputs, sort_keys=True).encode()).hexdigest()
    (res / 'cache-key.txt').write_text(cache_key + '\n')
    copy_notices(res, system, runtime, sdk)
    source = copy_source(res)
    write_manifest(
        bundle, res, source, system, obs_version, tensorrt_enabled, models, runtime_hash, cache_key, environment
    )
    if system == 'macos':
        seal_payload(bundle, system)
        sign_bundle(bundle, res)
        audit_payload(bundle, system)
    return bundle


# ---------------------------------------------------------------------------
# Build orchestration


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        '--platform', choices=['macos', 'windows'], default='windows' if sys.platform == 'win32' else 'macos'
    )
    local_cmake = Path(sys.executable).with_name('cmake.exe' if sys.platform == 'win32' else 'cmake')
    p.add_argument('--cmake', default=str(local_cmake) if local_cmake.is_file() else shutil.which('cmake') or 'cmake')
    p.add_argument('--build-dir', type=Path)
    p.add_argument('--output', type=Path)
    p.add_argument('--obs-app', type=Path, default=Path('/Applications/OBS.app'))
    p.add_argument('--obs-sdk', type=Path, help='Windows OBS development SDK prefix')
    p.add_argument('--obs-version', default=BUILDSPEC['obs-studio']['version'])
    p.add_argument(
        '--ort-library', type=Path, help='macOS ONNX Runtime dylib (default: pinned release from buildspec.json)'
    )
    p.add_argument(
        '--ort-root', type=Path, help='Windows ONNX Runtime SDK root (lib/onnxruntime.lib and lib/onnxruntime.dll)'
    )
    p.add_argument('--cpu-only', action='store_true')
    p.add_argument(
        '--build-only',
        action='store_true',
        help='Configure, compile, and run CTest without staging models/runtime/source',
    )
    a = p.parse_args()
    system = host_system()
    if a.platform != system:
        p.error('Run the native build on its target OS. Source preparation can be done on either OS.')
    try:
        a.cmake, a.ctest, a.cmake_version = cmake_tools(a.cmake, system)
        if system == 'windows':
            validate_windows_inputs(a.obs_sdk, a.ort_root)
            if not a.obs_sdk or not a.ort_root:
                raise ValueError('--obs-sdk and --ort-root are required')
        elif platform.machine() != 'arm64':
            raise ValueError('The macOS package currently targets Apple silicon')
    except ValueError as error:
        p.error(str(error))
    return a


def host_system() -> str:
    return 'windows' if sys.platform == 'win32' else 'macos' if sys.platform == 'darwin' else ''


def configure_command(a: argparse.Namespace, system: str, build: Path) -> list[str]:
    """Use the same presets as direct CMake builds, with explicit local SDK overrides."""
    if system == 'macos':
        ninja = shutil.which('ninja') or str(Path(a.cmake).with_name('ninja'))
        command = [a.cmake, '--preset', 'macos', '-B', str(build), f'-DCMAKE_MAKE_PROGRAM={ninja}']
        command.append(f'-DBEAUTI_OBS_APP={a.obs_app.resolve()}')
        if a.ort_library:
            command.append(f'-DBEAUTI_ORT_LIBRARY={a.ort_library.resolve()}')
        return command
    ort_root = a.ort_root.resolve()
    return [
        a.cmake,
        '--preset',
        'windows-cpu' if a.cpu_only else 'windows',
        '-S',
        str(ROOT),
        '-B',
        str(build),
        f'-DCMAKE_PREFIX_PATH={a.obs_sdk.resolve()}',
        f'-DBEAUTI_ORT_LIBRARY={ort_root}/lib/onnxruntime.lib',
        f'-DBEAUTI_ORT_RUNTIME={ort_root}/lib/onnxruntime.dll',
    ]


def tensorrt_pins() -> dict[str, str]:
    """Users install the NVIDIA runtime themselves; packages only record the pinned identities."""
    return {
        name: sha for spec in BUILDSPEC['tensorrt-rtx']['files'].values() for name, sha in spec['dll_sha256'].items()
    }


def finish_windows_bundle(bundle: Path) -> None:
    seal_payload(bundle, 'windows')
    run(sys.executable, '-m', 'beauti_tools.packaging.audit_windows_bundle', bundle)


def publish(bundle: Path, destination: Path) -> None:
    """Stage the bundle; the one it replaces survives as <name>.previous for A/B checks (one copy)."""
    with replacement_directory(destination, keep_previous=True) as staged:
        shutil.copytree(bundle, staged, symlinks=True, copy_function=shutil.copy, dirs_exist_ok=True)
        audit_snapshot(staged, (bundle / 'Contents/Resources/bundle.json').exists())


def main() -> None:
    a = parse_args()
    system = host_system()
    default_build, default_output = default_paths(ROOT, system, a.cpu_only)
    build = (a.build_dir or default_build).resolve()
    build.mkdir(parents=True, exist_ok=True)
    run(*configure_command(a, system, build))
    run(a.cmake, '--build', build, '--config', 'Release', '--parallel')
    run(a.ctest, '--test-dir', build, '-C', 'Release', '--output-on-failure')
    environment = build_environment(build, a.cmake_version)
    if a.build_only:
        print(f'Beauti {VERSION}: build and CTest passed; package staging skipped ({build})', flush=True)
        return
    cache = cmake_cache(build)
    if system == 'macos':
        runtime, sdk = Path(cache['BEAUTI_ORT_LIBRARY']), Path(cache['BEAUTI_OBS_SOURCE'])
    else:
        # CMake may advance a managed cache path after a pin update. Stage that
        # resolved runtime and its providers, not an older command-line location.
        runtime, sdk = Path(cache['BEAUTI_ORT_RUNTIME']), a.obs_sdk.resolve()
        a.ort_root = runtime.parent.parent
    # Sign outside cloud-synchronized source folders, which may add Finder metadata.
    with tempfile.TemporaryDirectory(prefix='beauti-stage-') as stage_dir:
        stage = Path(stage_dir)
        run(a.cmake, '--install', build, '--config', 'Release', '--prefix', stage)
        bundle = assemble(
            stage, system, runtime, a.obs_version, sdk, system == 'windows' and not a.cpu_only, environment
        )
        if system == 'windows':
            finish_windows_bundle(bundle)
        destination = (a.output or default_output).resolve()
        publish(bundle, destination)
    print(f'Beauti {VERSION}: {destination}', flush=True)


if __name__ == '__main__':
    main()
