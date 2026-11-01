# SPDX-License-Identifier: GPL-3.0-or-later
"""Incomplete or modified bundles cannot pass packaging with a stale verification label."""

import hashlib
import json
import sys
import zipfile

import pytest

from beauti_tools.packaging import package
from beauti_tools.packaging.integrity import MAC_BINARIES, audit_payload, digest, resources, seal_payload


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding='utf-8')


def make_bundle(tmp_path, system):
    bundle = tmp_path / ('beauti' if system == 'windows' else 'beauti.plugin')
    res = resources(bundle, system)
    binaries = MAC_BINARIES if system == 'macos' else {'beauti.dll', 'beauti-bench.exe', 'onnxruntime.dll'}
    for name in binaries:
        write(bundle / name, 'native binary fixture')
    if system == 'macos':
        write(bundle / 'Contents/Info.plist', 'plist fixture')
    for name in ['beauti.effect', 'continuity.effect', 'locale/en-US.ini', 'locale/de-DE.ini']:
        write(res / name, 'resource fixture')
        write(res / 'source/data' / name, 'resource fixture')
    models = {}
    for index in range(8):
        name = f'model-{index}.onnx'
        write(res / 'models' / name, 'model identity fixture')
        models[name] = digest(res / 'models' / name)
    runtime = digest(
        bundle / ('onnxruntime.dll' if system == 'windows' else 'Contents/Frameworks/libbeauti_onnxruntime.dylib')
    )
    key = hashlib.sha256(json.dumps([models, runtime], sort_keys=True).encode()).hexdigest()
    write(res / 'cache-key.txt', key)
    write(res / 'source/VERSION', '0.10.0\n')
    write(res / 'source/models/manifest.json', json.dumps({'files': models}))
    write(res / 'source/docs/provenance.json', '{}')
    write(res / 'source/buildspec.json', '{}')
    write(res / 'LICENSES/COPYING', 'license fixture')
    manifest = {
        'version': '0.10.0',
        'platform': system,
        'tensorrt_enabled': False,
        'model_sha256': models,
        'runtime_sha256': runtime,
        'cache_key': key,
        'provenance_sha256': digest(res / 'source/docs/provenance.json'),
        'verification': {'status': 'matched'},
    }
    write(res / 'bundle.json', json.dumps(manifest))
    if system == 'windows':
        runtime_files = [bundle / name for name in binaries]
        runtime_files += [
            p
            for p in res.rglob('*')
            if p.is_file() and p.relative_to(res).parts[0] not in {'source', 'LICENSES'} and p.name != 'bundle.json'
        ]
        record = {p.relative_to(bundle).as_posix(): digest(p) for p in runtime_files}
        write(res / 'verification.json', json.dumps({'passed': False, 'artifact_sha256': {'cpu': record}}))
    seal_payload(bundle, system)
    return bundle


@pytest.fixture(params=['windows', 'macos'])
def staged(request, tmp_path):
    return make_bundle(tmp_path, request.param), request.param


def test_complete_bundle_and_test_result_are_distinct(staged):
    bundle, system = staged
    result = audit_payload(bundle, system)
    assert result['passed']
    if system == 'windows':
        assert result['verification']['status'] == 'matched'
        assert result['verification']['reported_passed'] is False
    else:
        assert result['verification']['status'] == 'missing'


@pytest.mark.parametrize(
    'name',
    [
        'models/model-3.onnx',
        'beauti.effect',
        'continuity.effect',
        'locale/de-DE.ini',
        'cache-key.txt',
        'LICENSES/COPYING',
        'source/buildspec.json',
    ],
)
@pytest.mark.parametrize('change', ['remove', 'modify'])
def test_missing_or_modified_payload_is_rejected(staged, name, change):
    bundle, system = staged
    path = resources(bundle, system) / name
    if change == 'remove':
        path.unlink()
    else:
        path.write_bytes(b'changed after staging')
    with pytest.raises(ValueError, match='Bundle payload mismatch'):
        audit_payload(bundle, system)


def test_empty_model_directory_cannot_be_audited_or_sealed(staged):
    bundle, system = staged
    for path in (resources(bundle, system) / 'models').iterdir():
        path.unlink()
    with pytest.raises(ValueError, match='Bundle payload mismatch'):
        audit_payload(bundle, system)
    with pytest.raises(ValueError, match='Missing or unexpected model files'):
        seal_payload(bundle, system)


def test_unrecorded_file_is_rejected(staged):
    bundle, system = staged
    (bundle / 'unexpected-provider.dll').write_bytes(b'extra runtime')
    with pytest.raises(ValueError, match='Bundle payload mismatch'):
        audit_payload(bundle, system)


@pytest.mark.parametrize('name', ['beauti.dll', 'beauti-bench.exe', 'onnxruntime.dll'])
def test_changed_windows_binary_is_rejected(tmp_path, name):
    bundle = make_bundle(tmp_path, 'windows')
    (bundle / name).write_bytes(b'changed binary')
    with pytest.raises(ValueError, match='Bundle payload mismatch'):
        audit_payload(bundle, 'windows')


def test_inventory_is_required_even_with_old_matched_label(staged):
    bundle, system = staged
    path = resources(bundle, system) / 'bundle.json'
    manifest = json.loads(path.read_text())
    del manifest['payload_sha256']
    write(path, json.dumps(manifest))
    with pytest.raises(ValueError, match='restage'):
        audit_payload(bundle, system)


def test_native_mac_signing_does_not_invalidate_resource_inventory(tmp_path):
    bundle = make_bundle(tmp_path, 'macos')
    for name in MAC_BINARIES:
        (bundle / name).write_bytes(b'changed by signing; codesign verification is a separate required gate')
    write(bundle / 'Contents/_CodeSignature/CodeResources', 'signature fixture')
    assert audit_payload(bundle, 'macos')['passed']
    (bundle / 'Contents/MacOS/beauti').unlink()
    with pytest.raises(ValueError, match='Missing or empty native binary'):
        audit_payload(bundle, 'macos')


def test_packager_rechecks_the_copied_snapshot_and_report(tmp_path, monkeypatch):
    bundle = make_bundle(tmp_path, 'windows')
    res = resources(bundle, 'windows')
    report = json.loads((res / 'verification.json').read_text())
    report['artifact_sha256']['cpu']['beauti.dll'] = '0' * 64
    write(res / 'verification.json', json.dumps(report))
    seal_payload(bundle, 'windows')
    checked = []

    def check_snapshot(snapshot, mac):
        assert snapshot != bundle and not mac
        checked.append(snapshot)
        return {'integrity': audit_payload(snapshot, 'windows')}

    monkeypatch.setattr(package, 'audit_snapshot', check_snapshot)
    output = tmp_path / 'archives'
    monkeypatch.setattr(sys, 'argv', ['beauti-package', str(bundle), '--output-dir', str(output)])
    package.main()
    assert len(checked) == 1
    with zipfile.ZipFile(next(output.glob('*.zip'))) as archive:
        readme = archive.read(next(n for n in archive.namelist() if n.endswith('/README.txt'))).decode('utf-8')
    assert 'rechecked during packaging): historical' in readme
    assert 'Included report passed: False' in readme
    assert 'includes TensorRT RTX' not in readme


def test_packager_stops_before_archiving_a_damaged_copy(tmp_path, monkeypatch):
    bundle = make_bundle(tmp_path, 'windows')

    def damage_and_check(snapshot, mac):
        assert snapshot != bundle and not mac
        (snapshot / 'data/models/model-0.onnx').unlink()
        return {'integrity': audit_payload(snapshot, 'windows')}

    monkeypatch.setattr(package, 'audit_snapshot', damage_and_check)
    output = tmp_path / 'archives'
    monkeypatch.setattr(sys, 'argv', ['beauti-package', str(bundle), '--output-dir', str(output)])
    with pytest.raises(ValueError, match='Bundle payload mismatch'):
        package.main()
    assert not list(output.glob('*.zip'))
    assert audit_payload(bundle, 'windows')['passed']
