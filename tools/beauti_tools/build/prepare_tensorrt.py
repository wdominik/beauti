# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage the pinned NVIDIA Windows runtime DLLs for local native tests.

Packages do not contain them; users install them as described in docs/INSTALL.md.
buildspec.json (`tensorrt-rtx`) pins the archive, the DLL names, sizes, and hashes.
No system CUDA Toolkit, driver installation, model export, or PATH change is made.
"""

import argparse
import json
import shutil
import zipfile
from pathlib import Path
from typing import Any

from ..paths import ROOT
from .prepare_windows import download, sha
from .staging import replacement_directory

# 'acknowledg' matches the archive's acknowledgment file under either spelling.
NOTICE_WORDS = ('license', 'notice', 'eula', 'acknowledg', 'privacy', 'readme')


def stage_component(archive: Path, component: str, spec: dict[str, Any], destination: Path) -> dict[str, str]:
    """Extract the listed DLLs and every notice from one archive; return the DLL digests."""
    wanted = set(spec['dlls'])
    notices = destination / 'LICENSES' / component
    notices.mkdir(parents=True, exist_ok=True)
    records: dict[str, str] = {}
    notice_count = 0
    with zipfile.ZipFile(archive) as package:
        for entry in package.infolist():
            if entry.is_dir():
                continue
            name = Path(entry.filename).name
            if name in wanted:
                target = destination / name
            elif any(word in name.lower() for word in NOTICE_WORDS):
                target = notices / name
                notice_count += 1
            else:
                continue
            temporary = target.with_suffix(target.suffix + '.partial')
            with package.open(entry) as source, temporary.open('wb') as output:
                shutil.copyfileobj(source, output)
            digest = sha(temporary)
            if target.exists() and sha(target) != digest:
                raise RuntimeError('Conflicting runtime file: ' + str(target))
            temporary.replace(target)
            if name in wanted:
                records[name] = digest
    for name, digest in records.items():
        if digest != spec.get('dll_sha256', {}).get(name, digest):
            raise RuntimeError(component + ': pinned hash differs for ' + name)
    missing = sorted(wanted - set(records))
    if missing:
        raise RuntimeError(component + ': archive lacks ' + ', '.join(missing))
    if not notice_count:
        raise RuntimeError('Missing notices for ' + component)
    return records


def prepare_runtime(lock_path: Path, downloads: Path, destination: Path) -> dict[str, str]:
    """Validate a complete replacement before touching the previous pinned runtime."""
    lock = json.loads(lock_path.read_text(encoding='utf-8'))['dependencies']['tensorrt-rtx']
    downloads.mkdir(parents=True, exist_ok=True)
    with replacement_directory(destination) as staged:
        records: dict[str, str] = {}
        for component, spec in lock['files'].items():
            print('Preparing ' + component + ' ' + spec['version'], flush=True)
            files = stage_component(download(spec, downloads), component, spec, staged)
            records.update(files)
            print(f'Verified {component}: {len(files)} DLLs', flush=True)
        shutil.copyfile(lock_path, staged / 'LICENSES/buildspec.json')
        (staged / 'LICENSES/runtime-files.json').write_text(
            json.dumps(records, indent=2) + '\n', encoding='utf-8', newline='\n'
        )
    return records


def validated_files(directory: Path, spec: dict[str, Any]) -> list[Path]:
    """Reject stale pins, incomplete preparation, and altered DLLs before compiling."""
    try:
        notices = directory / 'LICENSES'
        prepared = json.loads((notices / 'buildspec.json').read_text(encoding='utf-8'))['dependencies']['tensorrt-rtx']
        records = json.loads((notices / 'runtime-files.json').read_text(encoding='utf-8'))
        names = [name for component in spec['files'].values() for name in component['dlls']]
        if prepared != spec or set(records) != set(names):
            raise ValueError('Prepared TensorRT RTX pin differs from buildspec.json; prepare it again')
        files = [directory / name for name in names]
        for file in files:
            if not file.is_file() or sha(file) != records[file.name]:
                raise ValueError('Missing or modified TensorRT RTX DLL: ' + file.name)
        for component in spec['files']:
            if not any(p.is_file() for p in (notices / component).rglob('*')):
                raise ValueError('Missing TensorRT RTX notices: ' + component)
        return files
    except (OSError, KeyError) as error:
        raise ValueError('Incomplete TensorRT RTX runtime; run beauti-prepare-tensorrt') from error


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / '.cache/windows/tensorrt-runtime')
    args = parser.parse_args()
    destination = args.output.resolve()
    records = prepare_runtime(ROOT / 'buildspec.json', ROOT / '.cache/windows/downloads', destination)
    print('TensorRT RTX runtime: ' + str(destination) + f' ({len(records)} DLLs)', flush=True)


if __name__ == '__main__':
    main()
