# SPDX-License-Identifier: GPL-3.0-or-later
"""The Windows NVIDIA runtime is the DLL subset pinned in buildspec.json, consistently everywhere."""

import json
import tempfile
import unittest
import zipfile
from pathlib import Path

import pytest

from beauti_tools.build.prepare_tensorrt import prepare_runtime, stage_component, validated_files
from beauti_tools.build.prepare_windows import sha
from beauti_tools.packaging.integrity import bundled_nvidia_files

ROOT = Path(__file__).resolve().parents[2]
LOCK = json.loads((ROOT / 'buildspec.json').read_text(encoding='utf-8'))['dependencies']['tensorrt-rtx']
SHIPPED = {dll for spec in LOCK['files'].values() for dll in spec['dlls']}


def archive(path: Path, entries: dict[str, bytes]) -> Path:
    with zipfile.ZipFile(path, 'w') as package:
        for name, data in entries.items():
            package.writestr(name, data)
    return path


def runtime_pin(tmp_path, entries):
    downloads = tmp_path / 'downloads'
    downloads.mkdir(exist_ok=True)
    package = archive(downloads / 'component.zip', entries)
    lock = tmp_path / 'buildspec.json'
    lock.write_text(
        json.dumps(
            {
                'dependencies': {
                    'tensorrt-rtx': {
                        'files': {
                            'component': {
                                'version': 'test',
                                'url': 'https://example.invalid/component.zip',
                                'sha256': sha(package),
                                'dlls': ['wanted64_9.dll'],
                            }
                        }
                    }
                }
            }
        )
    )
    return lock, downloads


def snapshot(path):
    return {p.relative_to(path).as_posix(): p.read_bytes() for p in path.rglob('*') if p.is_file()}


def test_pin_update_replaces_old_dlls_and_notices(tmp_path):
    destination = tmp_path / 'runtime'
    destination.mkdir()
    (destination / 'wanted64_9.dll').write_bytes(b'old version')
    (destination / 'unused64_9.dll').write_bytes(b'obsolete component')
    (destination / 'LICENSES/old-component').mkdir(parents=True)
    (destination / 'LICENSES/old-component/LICENSE').write_bytes(b'obsolete notice')
    lock, downloads = runtime_pin(tmp_path, {'bin/wanted64_9.dll': b'new version', 'LICENSE': b'current notice'})

    records = prepare_runtime(lock, downloads, destination)

    assert (destination / 'wanted64_9.dll').read_bytes() == b'new version'
    assert set(snapshot(destination)) == {
        'wanted64_9.dll',
        'LICENSES/component/LICENSE',
        'LICENSES/buildspec.json',
        'LICENSES/runtime-files.json',
    }
    assert json.loads((destination / 'LICENSES/runtime-files.json').read_text()) == records
    assert not list(tmp_path.glob('.staging-*'))


@pytest.mark.parametrize('missing', ['bin/wanted64_9.dll', 'LICENSE'])
def test_incomplete_update_preserves_previous_runtime(tmp_path, missing):
    destination = tmp_path / 'runtime'
    entries = {'bin/wanted64_9.dll': b'original', 'LICENSE': b'original notice'}
    lock, downloads = runtime_pin(tmp_path, entries)
    prepare_runtime(lock, downloads, destination)
    original = snapshot(destination)
    entries['bin/wanted64_9.dll'] = b'new version'
    del entries[missing]
    runtime_pin(tmp_path, entries)

    with pytest.raises(RuntimeError, match=r'archive lacks|Missing notices'):
        prepare_runtime(lock, downloads, destination)

    assert snapshot(destination) == original
    assert not list(tmp_path.glob('.staging-*'))


def test_failed_publish_restores_previous_runtime(tmp_path, monkeypatch):
    destination = tmp_path / 'runtime'
    lock, downloads = runtime_pin(tmp_path, {'bin/wanted64_9.dll': b'original', 'LICENSE': b'notice'})
    prepare_runtime(lock, downloads, destination)
    original = snapshot(destination)
    rename = Path.rename

    def fail_publish(path, target):
        if path.name == 'staged':
            raise PermissionError('injected publish failure')
        return rename(path, target)

    monkeypatch.setattr(Path, 'rename', fail_publish)
    with pytest.raises(PermissionError, match='injected publish failure'):
        prepare_runtime(lock, downloads, destination)

    assert snapshot(destination) == original
    assert not list(tmp_path.glob('.staging-*'))


@pytest.mark.parametrize('change', ['dll', 'pin', 'notices'])
def test_prepared_runtime_is_checked_before_use(tmp_path, change):
    destination = tmp_path / 'runtime'
    lock, downloads = runtime_pin(tmp_path, {'bin/wanted64_9.dll': b'original', 'LICENSE': b'notice'})
    prepare_runtime(lock, downloads, destination)
    spec = json.loads(lock.read_text())['dependencies']['tensorrt-rtx']
    assert validated_files(destination, spec) == [destination / 'wanted64_9.dll']
    if change == 'dll':
        (destination / 'wanted64_9.dll').write_bytes(b'modified')
    elif change == 'pin':
        spec['files']['component']['version'] = 'new'
    else:
        (destination / 'LICENSES/component/LICENSE').unlink()
    with pytest.raises(ValueError, match='TensorRT RTX'):
        validated_files(destination, spec)


class TensorRTRuntimeTests(unittest.TestCase):
    def test_staging_takes_only_the_listed_dlls_and_the_notices(self):
        with tempfile.TemporaryDirectory() as path:
            source = archive(
                Path(path) / 'component.zip',
                {
                    'component/bin/wanted64_9.dll': b'wanted',
                    'component/bin/unused64_9.dll': b'unused',
                    'component/lib/wanted64_9.lib': b'import library',
                    'component/LICENSE': b'notice',
                },
            )
            destination = Path(path) / 'runtime'
            records = stage_component(source, 'component', {'dlls': ['wanted64_9.dll']}, destination)
            self.assertEqual(sorted(records), ['wanted64_9.dll'])
            self.assertEqual(sorted(p.name for p in destination.iterdir()), ['LICENSES', 'wanted64_9.dll'])
            self.assertTrue((destination / 'LICENSES/component/LICENSE').is_file())

    def test_a_listed_dll_missing_from_the_archive_fails(self):
        with tempfile.TemporaryDirectory() as path:
            source = archive(Path(path) / 'component.zip', {'component/LICENSE': b'notice'})
            with self.assertRaisesRegex(RuntimeError, 'lacks wanted64_9.dll'):
                stage_component(source, 'component', {'dlls': ['wanted64_9.dll']}, Path(path) / 'runtime')

    def test_exactly_the_provider_distribution_is_pinned(self):
        self.assertEqual(list(LOCK['files']), ['tensorrt-rtx'])
        self.assertEqual(len(SHIPPED), 4)
        self.assertIn('onnxruntime_providers_nv_tensorrt_rtx.dll', SHIPPED)
        self.assertTrue(all(name.endswith('.dll') for name in SHIPPED))


def test_user_install_guide_names_the_pinned_runtime():
    guide = (ROOT / 'docs/INSTALL.md').read_text(encoding='utf-8')
    spec = LOCK['files']['tensorrt-rtx']
    assert spec['url'].rsplit('/', 1)[1] in guide
    assert LOCK['version'] in guide
    for name in spec['dlls']:
        assert name in guide
    assert set(spec['dll_bytes']) == set(spec['dll_sha256']) == set(spec['dlls'])


def test_audit_rejects_a_package_with_the_nvidia_runtime(tmp_path):
    (tmp_path / 'beauti.dll').write_bytes(b'plugin')
    assert not bundled_nvidia_files(tmp_path, LOCK)
    (tmp_path / 'data').mkdir()
    (tmp_path / 'data/TensorRT_RTX_1_7.dll').write_bytes(b'runtime')
    assert bundled_nvidia_files(tmp_path, LOCK) == ['data/TensorRT_RTX_1_7.dll']


if __name__ == '__main__':
    unittest.main()
