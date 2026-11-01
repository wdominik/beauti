# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a version's release notes from CHANGELOG.md for the GitHub release."""

import argparse
import re
from pathlib import Path

from ..paths import ROOT

FOOTER = """
---

**Installation:** see the [installation guide](https://github.com/wdominik/beauti/blob/v{version}/docs/INSTALL.md).
On Windows, NVIDIA acceleration needs files from NVIDIA that you add yourself.

These builds are not signed: macOS needs one extra step, which the guide describes, and
Windows may show a warning. Each package includes its corresponding source.
"""


def changelog_section(changelog: str, version: str) -> str:
    """A version without a section was not prepared for release."""
    match = re.search(rf'^## \[{re.escape(version)}\][^\n]*\n(.*?)(?=^## \[|\Z)', changelog, re.MULTILINE | re.DOTALL)
    if not match or not match[1].strip():
        raise ValueError('CHANGELOG.md has no section for ' + version)
    return match[1].strip() + '\n'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('version')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    section = changelog_section((ROOT / 'CHANGELOG.md').read_text(encoding='utf-8'), args.version)
    args.output.write_text(section + FOOTER.format(version=args.version), encoding='utf-8')


if __name__ == '__main__':
    main()
