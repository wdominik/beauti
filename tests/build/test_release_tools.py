# SPDX-License-Identifier: GPL-3.0-or-later
"""Release helpers: the model archive and the notes taken from the changelog."""

import hashlib
import json
import zipfile

import pytest

from beauti_tools.models.archive import NOTICES, pack, release_tag, unpack
from beauti_tools.packaging.release_notes import FOOTER, changelog_section
from beauti_tools.paths import ROOT


@pytest.fixture
def models(tmp_path):
    folder = tmp_path / 'models'
    (folder / 'native').mkdir(parents=True)
    (folder / 'native/a.onnx').write_bytes(b'model a')
    (folder / 'b.onnx').write_bytes(b'model b')
    licenses = tmp_path / 'licenses'
    licenses.mkdir()
    for name in NOTICES:
        (licenses / name).write_text('notice ' + name)
    files = {
        'native/a.onnx': hashlib.sha256(b'model a').hexdigest(),
        'b.onnx': hashlib.sha256(b'model b').hexdigest(),
    }
    return folder, licenses, files


def test_archive_round_trip_carries_the_notices(tmp_path, models):
    folder, licenses, files = models
    archive = tmp_path / 'out/models.zip'
    pack(files, folder, licenses, archive)
    with zipfile.ZipFile(archive) as content:
        assert {'licenses/' + name for name in NOTICES} <= set(content.namelist())
    target = tmp_path / 'unpacked'
    unpack(files, archive, target)
    assert (target / 'native/a.onnx').read_bytes() == b'model a'
    assert not list(target.rglob('*.partial'))


def test_pack_refuses_a_model_that_differs_from_the_manifest(tmp_path, models):
    folder, licenses, files = models
    (folder / 'b.onnx').write_bytes(b'changed')
    with pytest.raises(ValueError, match=r'b\.onnx'):
        pack(files, folder, licenses, tmp_path / 'models.zip')


def test_unpack_refuses_a_changed_or_incomplete_archive(tmp_path, models):
    folder, licenses, files = models
    archive = tmp_path / 'models.zip'
    pack(files, folder, licenses, archive)
    changed = dict(files, **{'b.onnx': hashlib.sha256(b'other').hexdigest()})
    with pytest.raises(ValueError, match='differs'):
        unpack(changed, archive, tmp_path / 'first')
    assert not (tmp_path / 'first/b.onnx').exists()
    with pytest.raises(ValueError, match='lacks'):
        unpack({'missing.onnx': 'x'}, archive, tmp_path / 'second')


def test_release_tag_follows_the_model_set():
    assert release_tag({'a': '1', 'b': '2'}) == release_tag({'b': '2', 'a': '1'})
    assert release_tag({'a': '1'}) != release_tag({'a': '2'})


def test_changelog_section_is_exactly_that_version():
    changelog = '# Changelog\n\n## [Unreleased]\n\n## [1.1.0] - 2026\n\nNew.\n\n## [1.0.0] - 2025\n\nOld.\n'
    assert changelog_section(changelog, '1.1.0') == 'New.\n'
    assert changelog_section(changelog, '1.0.0') == 'Old.\n'
    for missing in ('Unreleased', '2.0.0'):
        with pytest.raises(ValueError, match='no section'):
            changelog_section(changelog, missing)


def test_the_current_version_can_be_released():
    version = (ROOT / 'VERSION').read_text().strip()
    assert changelog_section((ROOT / 'CHANGELOG.md').read_text(encoding='utf-8'), version)
    assert '{version}' in FOOTER
    assert json.loads((ROOT / 'models/manifest.json').read_text())['files']
