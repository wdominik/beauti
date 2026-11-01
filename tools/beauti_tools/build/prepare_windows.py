# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare pinned Windows dependencies without running Windows binaries.
On Windows, additionally generate x64 import libraries with Microsoft's lib.exe.
The OBS SDK uses official matching headers and DLL export tables, not a locally
rebuilt OBS app. --download-only is available on macOS for preparation/auditing.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

from ..paths import ROOT
from .staging import replacement_directory


def sha(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def download(spec: dict[str, str], folder: Path) -> Path:
    path = folder / spec['url'].rsplit('/', 1)[-1]
    if not path.exists():
        temporary = path.with_suffix(path.suffix + '.partial')
        with urllib.request.urlopen(spec['url'], timeout=60) as incoming, temporary.open('wb') as out:
            shutil.copyfileobj(incoming, out)
        if sha(temporary) != spec['sha256']:
            raise RuntimeError('Download checksum mismatch: ' + path.name)
        temporary.replace(path)
    if sha(path) != spec['sha256']:
        raise RuntimeError('Cached checksum mismatch: ' + path.name)
    return path


def extract(archive: Path, destination: Path) -> None:
    marker = destination / '.complete'
    digest = sha(archive)
    if marker.exists() and marker.read_text() == digest:
        return
    # Overlaying a new pin can retain old archive roots, which the SDK discovery
    # could then select. Replace the entire verified extraction instead.
    with replacement_directory(destination) as staged:
        if archive.name.endswith('.zip'):
            with zipfile.ZipFile(archive) as z:
                for item in z.infolist():
                    path = staged / item.filename
                    if not path.resolve().is_relative_to(staged.resolve()):
                        raise RuntimeError('Unsafe archive path')
                z.extractall(staged)
        else:
            with tarfile.open(archive) as t:
                t.extractall(staged, filter='data')
        (staged / '.complete').write_text(digest)


def import_definition(dll: Path, destination: Path) -> int:
    import pefile  # noqa: PLC0415 -- Windows-only runtime dependency; the dev group provides it elsewhere

    pe = pefile.PE(str(dll))
    if pe.FILE_HEADER.Machine != 0x8664:
        raise RuntimeError('Expected x64 DLL: ' + str(dll))
    exports = ['  ' + item.name.decode('ascii') for item in pe.DIRECTORY_ENTRY_EXPORT.symbols if item.name]
    destination.write_text('LIBRARY ' + dll.name + '\nEXPORTS\n' + '\n'.join(exports) + '\n')
    pe.close()
    return len(exports)


def microsoft_lib() -> str:
    found = shutil.which('lib.exe')
    if found:
        return found
    where = (
        Path(os.environ.get('PROGRAMFILES(X86)', 'C:/Program Files (x86)'))
        / 'Microsoft Visual Studio/Installer/vswhere.exe'
    )
    if not where.exists():
        raise RuntimeError('Install Visual Studio C++ Build Tools or use its x64 Native Tools prompt')
    root = subprocess.check_output(
        [
            str(where),
            '-latest',
            '-products',
            '*',
            '-requires',
            'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
            '-property',
            'installationPath',
        ],
        text=True,
    ).strip()
    candidates = sorted((Path(root) / 'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/lib.exe'))
    if not candidates:
        raise RuntimeError('Microsoft x64 lib.exe was not found')
    return str(candidates[-1])


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=ROOT / '.cache/windows')
    p.add_argument('--device', choices=['cpu', 'tensorrt'], default='tensorrt')
    p.add_argument('--download-only', action='store_true')
    a = p.parse_args()
    if sys.platform != 'win32' and not a.download_only:
        p.error('Use --download-only on non-Windows hosts')
    out = a.output.resolve()
    cache = out / 'downloads'
    cache.mkdir(parents=True, exist_ok=True)
    spec = json.loads((ROOT / 'buildspec.json').read_text(encoding='utf-8'))['dependencies']
    archives = {
        'obs_source': spec['obs-studio']['sources'],
        'obs_windows': spec['obs-studio']['windows'],
        'ort_cpu': spec['onnxruntime']['windows-x64'],
    }
    locations = {}
    for key, entry in archives.items():
        archive = download(entry, cache)
        destination = out / key
        extract(archive, destination)
        locations[key] = destination
        print('Verified ' + archive.name, flush=True)
    sdk = out / 'sdk'
    (sdk / 'lib').mkdir(parents=True, exist_ok=True)
    source = next(locations['obs_source'].glob('*/libobs/obs.h')).parents[1]
    extract(download(spec['simde'], cache), out / 'simde')
    simde = out / 'simde' / spec['simde']['archive_root']
    shutil.copyfile(source / 'COPYING', sdk / 'COPYING')
    shutil.copyfile(simde / 'COPYING', sdk / 'SIMDe-MIT.txt')
    (source / 'libobs/obsconfig.h').write_text('#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n')
    bins = locations['obs_windows'] / 'bin/64bit'
    counts = {}
    for name in ['obs', 'obs-frontend-api']:
        dll = bins / (name + '.dll')
        definition = sdk / 'lib' / (name + '.def')
        counts[name] = import_definition(dll, definition)
        if not a.download_only:
            subprocess.run(
                [
                    microsoft_lib(),
                    '/nologo',
                    '/machine:x64',
                    '/def:' + str(definition),
                    '/out:' + str(sdk / 'lib' / (name + '.lib')),
                ],
                check=True,
            )
    for package, target, name, includes in [
        ('libobs', 'OBS::libobs', 'obs', [source / 'libobs', simde, source / 'deps/w32-pthreads']),
        ('obs-frontend-api', 'OBS::obs-frontend-api', 'obs-frontend-api', [source / 'frontend/api']),
    ]:
        folder = sdk / 'lib/cmake' / package
        folder.mkdir(parents=True, exist_ok=True)
        text = (
            f'if(NOT TARGET {target})\n'
            f'  add_library({target} SHARED IMPORTED)\n'
            f'  set_target_properties({target} PROPERTIES\n'
            f'    IMPORTED_IMPLIB "{(sdk / "lib" / (name + ".lib")).as_posix()}"\n'
            f'    IMPORTED_LOCATION "{(bins / (name + ".dll")).as_posix()}"\n'
            '    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "'
            + ';'.join(f.as_posix() for f in includes)
            + '"\n    INTERFACE_INCLUDE_DIRECTORIES "'
            + ';'.join(f.as_posix() for f in includes)
            + '")\nendif()\n'
        )
        (folder / (package + 'Config.cmake')).write_text(text)
    ort = next(locations['ort_cpu'].glob('*/lib/onnxruntime.lib')).parents[1]
    report = {
        'obs_sdk': str(sdk),
        'obs_runtime': str(locations['obs_windows']),
        'ort_root': str(ort),
        'device': a.device,
        'export_counts': counts,
        'import_libraries_generated': not a.download_only,
        'windows_execution_verified': False,
    }
    (out / 'prepared.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
