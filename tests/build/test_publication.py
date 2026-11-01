# SPDX-License-Identifier: GPL-3.0-or-later
"""A failed build/archive update must preserve the last complete output."""

import hashlib
import sys
import zipfile
from pathlib import Path

import pytest
from test_bundle_integrity import make_bundle

from beauti_tools.build import build
from beauti_tools.build.staging import replacement_directory, replacement_files
from beauti_tools.packaging import package
from beauti_tools.packaging.integrity import audit_payload


@pytest.fixture(params=['windows', 'macos'])
def publication(request, tmp_path, monkeypatch):
    system = request.param
    bundle = make_bundle(tmp_path / 'input', system)
    destination = tmp_path / 'output' / bundle.name
    destination.mkdir(parents=True)
    (destination / 'identity').write_text('current')
    backup = destination.with_name(destination.name + '.previous')
    backup.mkdir()
    (backup / 'identity').write_text('older')

    def audit(snapshot, mac):
        assert snapshot != bundle and snapshot.parent.parent == destination.parent
        assert mac == (system == 'macos')
        return {'integrity': audit_payload(snapshot, system)}

    monkeypatch.setattr(build, 'audit_snapshot', audit)
    return bundle, destination, backup


def test_publish_retains_one_previous_bundle(publication):
    bundle, destination, backup = publication
    build.publish(bundle, destination)
    assert (backup / 'identity').read_text() == 'current'
    assert not (destination / 'identity').exists()
    assert not list(destination.parent.glob('.staging-*'))


@pytest.mark.parametrize('failure', ['copy', 'audit', 'rename'])
def test_failed_bundle_publish_preserves_current_and_backup(publication, monkeypatch, failure):
    bundle, destination, backup = publication
    if failure == 'copy':

        def fail_copy(source, target, **kwargs):
            (target / 'partial').write_text('incomplete')
            raise OSError('injected copy failure')

        monkeypatch.setattr(build.shutil, 'copytree', fail_copy)
    elif failure == 'audit':

        def fail_audit(snapshot, mac):
            raise ValueError('injected audit failure')

        monkeypatch.setattr(build, 'audit_snapshot', fail_audit)
    else:
        rename = Path.rename

        def fail_rename(source, target):
            if source.name == 'staged':
                raise PermissionError('injected rename failure')
            return rename(source, target)

        monkeypatch.setattr(Path, 'rename', fail_rename)
    with pytest.raises((OSError, ValueError), match='injected'):
        build.publish(bundle, destination)
    assert (destination / 'identity').read_text() == 'current'
    assert (backup / 'identity').read_text() == 'older'
    assert not list(destination.parent.glob('.staging-*'))


def test_directory_rollback_failure_preserves_recoverable_bundle(tmp_path, monkeypatch):
    destination = tmp_path / 'bundle'
    destination.mkdir()
    (destination / 'identity').write_text('current')
    rename = Path.rename

    def fail_rename(source, target):
        if target == destination:
            raise PermissionError('injected publish and rollback failure')
        return rename(source, target)

    monkeypatch.setattr(Path, 'rename', fail_rename)
    with (
        pytest.raises(PermissionError, match='injected'),
        replacement_directory(destination, keep_previous=True) as staged,
    ):
        (staged / 'identity').write_text('new')
    assert next(tmp_path.glob('.staging-*/previous/identity')).read_text() == 'current'


@pytest.mark.parametrize('existing', [False, True])
def test_archive_pair_rollback_after_first_file_published(tmp_path, monkeypatch, existing):
    archive, checksum = tmp_path / 'bundle.zip', tmp_path / 'bundle.sha256'
    if existing:
        archive.write_bytes(b'old archive')
        checksum.write_bytes(b'old checksum')
    rename = Path.rename

    def fail_checksum(source, target):
        if source.name == checksum.name and source != checksum:
            raise PermissionError('injected checksum publication failure')
        return rename(source, target)

    monkeypatch.setattr(Path, 'rename', fail_checksum)
    with pytest.raises(PermissionError, match='injected'), replacement_files(archive, checksum) as (staged, digest):
        staged.write_bytes(b'new archive')
        digest.write_bytes(b'new checksum')
    if existing:
        assert archive.read_bytes() == b'old archive'
        assert checksum.read_bytes() == b'old checksum'
    else:
        assert not archive.exists() and not checksum.exists()
    assert not list(tmp_path.glob('.staging-*'))


def test_archive_rollback_failure_retains_recovery_files(tmp_path, monkeypatch):
    archive, checksum = tmp_path / 'bundle.zip', tmp_path / 'bundle.sha256'
    archive.write_bytes(b'old archive')
    checksum.write_bytes(b'old checksum')
    rename = Path.rename

    def fail_publish_and_restore(source, target):
        if source.parent != tmp_path:
            raise PermissionError('injected failure')
        return rename(source, target)

    monkeypatch.setattr(Path, 'rename', fail_publish_and_restore)
    with (
        pytest.raises(OSError, match='recovery files retained'),
        replacement_files(archive, checksum) as (staged, digest),
    ):
        staged.write_bytes(b'new archive')
        digest.write_bytes(b'new checksum')
    assert next(tmp_path.glob('.staging-*/previous-0')).read_bytes() == b'old archive'
    assert next(tmp_path.glob('.staging-*/previous-1')).read_bytes() == b'old checksum'


@pytest.mark.parametrize('failure', ['compression', 'crc', 'checksum', None])
def test_package_never_truncates_existing_archive(tmp_path, monkeypatch, failure):
    bundle = make_bundle(tmp_path / 'input', 'windows')
    output = tmp_path / 'archives'
    output.mkdir()
    archive = output / 'Beauti-Windows-0.10.0-x64.zip'
    checksum = archive.with_suffix('.sha256')
    archive.write_bytes(b'old archive')
    checksum.write_bytes(b'old checksum')
    monkeypatch.setattr(package, 'audit_snapshot', lambda p, _: {'integrity': audit_payload(p, 'windows')})
    monkeypatch.setattr(sys, 'argv', ['beauti-package', str(bundle), '--output-dir', str(output)])
    if failure == 'compression':

        def fail_write(*args, **kwargs):
            raise OSError('injected compression failure')

        monkeypatch.setattr(zipfile.ZipFile, 'write', fail_write)
    elif failure == 'crc':
        monkeypatch.setattr(zipfile.ZipFile, 'testzip', lambda _: 'bad-file')
    elif failure == 'checksum':
        write = Path.write_text

        def fail_checksum(path, *args, **kwargs):
            if path.suffix == '.sha256':
                raise OSError('injected checksum failure')
            return write(path, *args, **kwargs)

        monkeypatch.setattr(Path, 'write_text', fail_checksum)
    if failure:
        with pytest.raises((OSError, ValueError)):
            package.main()
        assert archive.read_bytes() == b'old archive'
        assert checksum.read_bytes() == b'old checksum'
    else:
        package.main()
        with zipfile.ZipFile(archive) as result:
            assert result.testzip() is None
        assert checksum.read_text().split()[0] == hashlib.sha256(archive.read_bytes()).hexdigest()
    assert not list(output.glob('.staging-*'))
