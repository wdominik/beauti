# SPDX-License-Identifier: GPL-3.0-or-later
"""Select versioned source files for provenance and source distribution."""

from collections.abc import Iterator
from pathlib import Path

ROOT_FILES = (
    'README.md',
    'CONTRIBUTING.md',
    'CHANGELOG.md',
    'VERSION',
    'COPYING',
    'CMakeLists.txt',
    'CMakePresets.json',
    '.gitignore',
    '.gitattributes',
    'buildspec.json',
    'pyproject.toml',
    'uv.lock',
    '.python-version',
    '.clang-format',
    '.editorconfig',
    '.pre-commit-config.yaml',
)
SOURCE_DIRS = ('src', 'data', 'cmake', 'tools', 'docs', 'models', 'licenses', 'third_party', 'tests')


def source_files(root: Path | str) -> Iterator[Path]:
    root = Path(root).resolve()
    for name in ROOT_FILES:
        path = root / name
        if path.is_file():
            yield path
    for name in SOURCE_DIRS:
        for path in sorted((root / name).rglob('*')):
            if any(part in ('__pycache__', '.DS_Store') for part in path.relative_to(root).parts):
                continue
            if path.is_symlink():
                raise RuntimeError('Source distributions cannot contain symlinks: ' + str(path.relative_to(root)))
            if path.is_file() and path.suffix not in ('.pyc', '.log'):
                yield path
