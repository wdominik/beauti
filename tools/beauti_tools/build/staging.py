# SPDX-License-Identifier: GPL-3.0-or-later
"""Publish prepared build outputs and dependencies with recoverable replacements."""

import shutil
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from uuid import uuid4


def staging_directory(parent: Path) -> Path:
    # Published outputs must inherit their destination's access permissions.
    # tempfile.mkdtemp uses owner-only Windows ACLs; renaming its
    # children would retain those ACLs and can make a shared build unreadable.
    temporary = parent / ('.staging-' + uuid4().hex)
    temporary.mkdir()
    return temporary


@contextmanager
def replacement_directory(destination: Path, *, keep_previous: bool = False) -> Iterator[Path]:
    """Stage beside the destination for same-volume renames, with rollback on publish failure."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = staging_directory(destination.parent)
    staged, previous = temporary / 'staged', temporary / 'previous'
    staged.mkdir()
    try:
        yield staged
        if destination.exists():
            destination.rename(previous)
        try:
            staged.rename(destination)
        except OSError:
            if previous.exists():
                previous.rename(destination)
            raise
        if previous.exists() and keep_previous:
            backup = destination.with_name(destination.name + '.previous')
            if backup.exists():
                shutil.rmtree(backup)
            previous.rename(backup)
        elif previous.exists():
            shutil.rmtree(previous)
    finally:
        # A failed rollback leaves the previous copy here for recovery. Never
        # let temporary-directory cleanup erase the only complete dependency.
        if not previous.exists():
            shutil.rmtree(temporary)


@contextmanager
def replacement_files(*destinations: Path) -> Iterator[list[Path]]:
    """Prepare related files beside their destinations; restore the old set on rename failure.

    Each rename is atomic, but the set is not a power-loss-atomic transaction.
    A failed rollback leaves recovery files in the reported staging directory.
    """
    parent = destinations[0].parent
    if any(path.parent != parent for path in destinations) or len(set(destinations)) != len(destinations):
        raise ValueError('Replacement files must be distinct siblings')
    parent.mkdir(parents=True, exist_ok=True)
    temporary = staging_directory(parent)
    staged = [temporary / path.name for path in destinations]
    backups: dict[Path, Path] = {}
    published: list[Path] = []
    try:
        yield staged
        if not all(path.is_file() for path in staged):
            raise ValueError('Every replacement file must be prepared before publication')
        try:
            for index, path in enumerate(destinations):
                if path.exists():
                    backup = temporary / f'previous-{index}'
                    path.rename(backup)
                    backups[path] = backup
            for source, path in zip(staged, destinations):
                source.rename(path)
                published.append(path)
        except OSError:
            try:
                for path in reversed(published):
                    path.unlink()
                for path, backup in backups.items():
                    backup.rename(path)
            except OSError as error:
                raise OSError(f'Publication rollback failed; recovery files retained in {temporary}') from error
            raise
        for backup in backups.values():
            backup.unlink()
    finally:
        if not any(path.exists() for path in backups.values()):
            shutil.rmtree(temporary)
