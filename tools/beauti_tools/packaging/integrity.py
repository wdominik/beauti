# SPDX-License-Identifier: GPL-3.0-or-later
"""Check staged payload identities separately from native linkage and test evidence.

macOS signs native code after this inventory is written. Its three native files
and the generated signature are checked by codesign instead of self-referential
hashes inside the signed bundle. All other payload files are hashed on both OSes.
"""

import hashlib
import json
from pathlib import Path
from typing import Any

from ..build.pipeline import verification_binding

MAC_BINARIES = {
    'Contents/MacOS/beauti',
    'Contents/Frameworks/libbeauti_onnxruntime.dylib',
    'Contents/Frameworks/libbeauti_coreai.dylib',
    'Contents/Resources/bin/beauti-bench',
}
# Models only one platform loads: TensorRT's float16 graphs and the Core AI assets.
PLATFORM_ONLY = {'windows': ('fp16/', 'native/fp16-fast/'), 'macos': ('coreai/',)}


def platform_models(files: dict[str, str], system: str) -> dict[str, str]:
    """The manifest entries a bundle for this platform ships."""
    other = [prefix for name, prefixes in PLATFORM_ONLY.items() if name != system for prefix in prefixes]
    return {name: sha for name, sha in files.items() if not name.startswith(tuple(other))}


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def resources(bundle: Path, system: str) -> Path:
    if system not in ('windows', 'macos'):
        raise ValueError('Unsupported bundle platform: ' + system)
    return bundle / ('data' if system == 'windows' else 'Contents/Resources')


def bundled_nvidia_files(bundle: Path, lock: dict[str, Any]) -> list[str]:
    """Users download NVIDIA's runtime themselves (docs/INSTALL.md); it never ships."""
    names = {name.lower() for spec in lock['files'].values() for name in spec['dlls']}
    return sorted(p.relative_to(bundle).as_posix() for p in bundle.rglob('*') if p.name.lower() in names)


def payload_files(bundle: Path, system: str) -> dict[str, Path]:
    """Inventory actual files, including notices/source; reject unrecorded additions."""
    manifest = resources(bundle, system) / 'bundle.json'
    files = {}
    for path in sorted(bundle.rglob('*')):
        if path.is_symlink():
            raise ValueError('Unexpected bundle symlink: ' + str(path.relative_to(bundle)))
        name = path.relative_to(bundle).as_posix()
        if path == manifest or (
            system == 'macos' and (name in MAC_BINARIES or name.startswith('Contents/_CodeSignature/'))
        ):
            continue
        if path.is_file():
            files[name] = path
    return files


def validate_layout(bundle: Path, manifest: dict[str, Any]) -> None:
    """Required models/resources come from the included source, not today's checkout."""
    system = manifest['platform']
    res = resources(bundle, system)
    binaries = MAC_BINARIES if system == 'macos' else {'beauti.dll', 'beauti-bench.exe', 'onnxruntime.dll'}
    for name in binaries:
        if not (bundle / name).is_file() or not (bundle / name).stat().st_size:
            raise ValueError('Missing or empty native binary: ' + name)
    source = res / 'source'
    if (source / 'VERSION').read_text(encoding='utf-8').strip() != manifest.get('version'):
        raise ValueError('Bundle version differs from the included source')
    if system == 'windows' and digest(bundle / 'onnxruntime.dll') != manifest.get('runtime_sha256'):
        raise ValueError('ONNX Runtime checksum mismatch')
    models = platform_models(json.loads((source / 'models/manifest.json').read_text(encoding='utf-8'))['files'], system)
    if not models or manifest.get('model_sha256') != models:
        raise ValueError('Model identities differ from the included source manifest')
    actual = {p.relative_to(res / 'models').as_posix() for p in (res / 'models').rglob('*') if p.is_file()}
    if actual != set(models):
        raise ValueError('Missing or unexpected model files: ' + ', '.join(sorted(actual ^ set(models))))
    for name, expected in models.items():
        if digest(res / 'models' / name) != expected:
            raise ValueError('Model checksum mismatch: ' + name)
    for name in ('beauti.effect', 'continuity.effect', 'locale/en-US.ini'):
        if not (source / 'data' / name).is_file():
            raise ValueError('Missing source resource: ' + name)
    for original in (source / 'data').rglob('*'):
        if original.is_file():
            target = res / original.relative_to(source / 'data')
            if not target.is_file() or digest(target) != digest(original):
                raise ValueError('Missing or modified resource: ' + str(target.relative_to(res)))
    cache_inputs = [models, manifest['runtime_sha256']]
    if manifest.get('tensorrt_enabled'):
        pin = json.loads((source / 'buildspec.json').read_text())['dependencies']['tensorrt-rtx']
        cache_inputs.append({name: sha for spec in pin['files'].values() for name, sha in spec['dll_sha256'].items()})
    key = hashlib.sha256(json.dumps(cache_inputs, sort_keys=True).encode()).hexdigest()
    if manifest.get('cache_key') != key or (res / 'cache-key.txt').read_text(encoding='utf-8').strip() != key:
        raise ValueError('Model cache key does not match the bundle identities')
    if digest(source / 'docs/provenance.json') != manifest.get('provenance_sha256'):
        raise ValueError('Source provenance checksum mismatch')
    if not any(p.is_file() for p in (res / 'LICENSES').rglob('*')):
        raise ValueError('Missing license notices')


def seal_payload(bundle: Path, system: str) -> None:
    """Record the completed staging output once; audits never repair or reseal it."""
    path = resources(bundle, system) / 'bundle.json'
    manifest = json.loads(path.read_text(encoding='utf-8'))
    if manifest.get('platform') != system:
        raise ValueError('Bundle platform mismatch')
    validate_layout(bundle, manifest)
    manifest['payload_sha256'] = {name: digest(file) for name, file in payload_files(bundle, system).items()}
    path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


def current_verification(bundle: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    """Report identity is recalculated; a byte match is not a passing test result."""
    path = resources(bundle, manifest['platform']) / 'verification.json'
    if not path.is_file():
        return {'status': 'missing'}
    report = json.loads(path.read_text(encoding='utf-8'))
    if manifest['platform'] == 'windows':
        result = verification_binding(bundle, report, 'tensorrt' if manifest['tensorrt_enabled'] else 'cpu')
    else:
        result = {'status': 'historical', 'reason': 'No macOS runtime artifact binding is recorded'}
    result['reported_passed'] = report.get('passed')
    return result


def audit_payload(bundle: Path, system: str) -> dict[str, Any]:
    manifest = json.loads((resources(bundle, system) / 'bundle.json').read_text(encoding='utf-8'))
    if manifest.get('platform') != system:
        raise ValueError('Bundle platform mismatch')
    expected = manifest.get('payload_sha256')
    if not isinstance(expected, dict) or not expected:
        raise ValueError('Missing payload inventory; restage with the current beauti-build')
    actual = payload_files(bundle, system)
    differences = set(actual) ^ set(expected)
    differences.update(name for name in actual.keys() & expected.keys() if digest(actual[name]) != expected[name])
    if differences:
        raise ValueError('Bundle payload mismatch: ' + ', '.join(sorted(differences)))
    validate_layout(bundle, manifest)
    return {'passed': True, 'files_checked': len(actual), 'verification': current_verification(bundle, manifest)}
