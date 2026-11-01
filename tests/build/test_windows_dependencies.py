# SPDX-License-Identifier: GPL-3.0-or-later
"""Dependency pin upgrades cannot leave an older SDK root or publish a partial archive."""

import zipfile

import pytest

from beauti_tools.build.prepare_windows import extract, sha


def test_new_archive_removes_old_sdk_root(tmp_path):
    destination = tmp_path / 'sdk'
    destination.mkdir()
    old = destination / 'old-release'
    old.mkdir()
    (old / 'obs.h').write_text('old headers')
    package = tmp_path / 'release.zip'
    with zipfile.ZipFile(package, 'w') as archive:
        archive.writestr('new-release/obs.h', 'new headers')

    extract(package, destination)

    assert not old.exists()
    assert (destination / 'new-release/obs.h').read_text() == 'new headers'
    assert (destination / '.complete').read_text() == sha(package)


def test_invalid_archive_preserves_previous_sdk(tmp_path):
    destination = tmp_path / 'sdk'
    destination.mkdir()
    (destination / 'obs.h').write_text('verified headers')
    package = tmp_path / 'invalid.zip'
    with zipfile.ZipFile(package, 'w') as archive:
        archive.writestr('new-release/obs.h', 'new headers')
        archive.writestr('../outside', 'unsafe')

    with pytest.raises(RuntimeError, match='Unsafe archive path'):
        extract(package, destination)

    assert sorted(p.name for p in destination.iterdir()) == ['obs.h']
    assert (destination / 'obs.h').read_text() == 'verified headers'
    assert not list(tmp_path.glob('.staging-*'))
