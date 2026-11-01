# SPDX-License-Identifier: GPL-3.0-or-later
"""The locale catalogs under data/locale/ are complete and well formed."""

from pathlib import Path

import check_locales

LOCALES = Path(__file__).resolve().parents[2] / 'data/locale'
EXPECTED = {'en-US', 'de-DE', 'es-ES', 'pt-BR', 'fr-FR', 'ru-RU', 'ja-JP', 'ko-KR', 'zh-CN', 'zh-TW'}


def test_every_catalog_has_every_key():
    report = check_locales.run(LOCALES)
    assert report['passed'], report['problems']


def test_the_shipped_languages_are_present():
    assert {p.stem for p in LOCALES.glob('*.ini')} == EXPECTED
