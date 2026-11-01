# SPDX-License-Identifier: GPL-3.0-or-later
"""Check payload identities and PE imports without executing Windows code."""

import argparse
import json
from pathlib import Path

import pefile

from .integrity import audit_payload, bundled_nvidia_files

SYSTEM = set(
    [
        'kernel32.dll',
        'kernelbase.dll',
        'ntdll.dll',
        'user32.dll',
        'gdi32.dll',
        'advapi32.dll',
        'ole32.dll',
        'oleaut32.dll',
        'shell32.dll',
        'shlwapi.dll',
        'ws2_32.dll',
        'bcrypt.dll',
        'crypt32.dll',
        'ncrypt.dll',
        'secur32.dll',
        'rpcrt4.dll',
        'version.dll',
        'winmm.dll',
        'psapi.dll',
        'dbghelp.dll',
        'setupapi.dll',
        'cfgmgr32.dll',
        'powrprof.dll',
        'dxgi.dll',
        'd3d11.dll',
        'd3d12.dll',
        'dxcore.dll',
        'd3dcompiler_47.dll',
        'ucrtbase.dll',
        'msvcrt.dll',
        'normaliz.dll',
        'winhttp.dll',
        'wininet.dll',
        'wldap32.dll',
        'combase.dll',
        'imm32.dll',
        'shcore.dll',
        'comdlg32.dll',
        'dwmapi.dll',
        'propsys.dll',
        'userenv.dll',
        'wintrust.dll',
        'cryptbase.dll',
        'iphlpapi.dll',
    ]
)
HOST = {'obs.dll', 'obs-frontend-api.dll'}
DRIVER = {'nvcuda.dll', 'nvml.dll'}
# ONNX Runtime 1.31 also imports the atomic-wait component of the x64 VC++ runtime.
CRT = {
    'vcruntime140.dll',
    'vcruntime140_1.dll',
    'msvcp140.dll',
    'msvcp140_1.dll',
    'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll',
    'concrt140.dll',
}


def imports(path):
    pe = pefile.PE(str(path), fast_load=True)
    try:
        if pe.FILE_HEADER.Machine != 0x8664:
            raise ValueError('Expected x64 PE: ' + path.name)
        pe.parse_data_directories(
            directories=[
                pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
                pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT'],
            ]
        )
        return sorted(
            {
                entry.dll.decode('ascii').lower()
                for attr in ['DIRECTORY_ENTRY_IMPORT', 'DIRECTORY_ENTRY_DELAY_IMPORT']
                for entry in getattr(pe, attr, [])
            }
        )
    finally:
        pe.close()


def audit(bundle):
    integrity = audit_payload(bundle, 'windows')
    directory = bundle  # OBS 33 layout: module and runtime DLLs in the plugin folder
    files = {p.name.lower(): p for p in directory.iterdir() if p.suffix.lower() in ('.dll', '.exe')}
    for name in ['beauti.dll', 'beauti-bench.exe', 'onnxruntime.dll']:
        if name not in files:
            raise ValueError('Missing binary: ' + name)
    allowed = SYSTEM | HOST | DRIVER | CRT
    missing = {}
    dependencies = {}
    for name, path in files.items():
        deps = imports(path)
        dependencies[name] = deps
        unresolved = [
            d for d in deps if d not in files and d not in allowed and not d.startswith(('api-ms-win-', 'ext-ms-win-'))
        ]
        if unresolved:
            missing[name] = unresolved
    lock = json.loads((bundle / 'data/source/buildspec.json').read_text(encoding='utf-8'))['dependencies']
    forbidden = bundled_nvidia_files(bundle, lock['tensorrt-rtx'])
    if forbidden:
        raise ValueError('The package must not contain the NVIDIA runtime: ' + ', '.join(forbidden))
    expected = {'beauti.dll', 'beauti-bench.exe', 'onnxruntime.dll'}
    stray = sorted(set(files) - expected)
    if stray:
        missing['Unexpected native binaries'] = stray
    if missing:
        raise ValueError('Unresolved runtime dependencies: ' + json.dumps(missing, indent=2))
    return {
        'passed': True,
        'integrity': integrity,
        'pe_files': len(files),
        'imports': dependencies,
        'external_prerequisites': [
            'OBS 33.0.0 x64 or later (plugin folder layout)',
            'Windows system libraries',
            'Microsoft Visual C++ runtime',
            'NVIDIA RTX GPU, CUDA 13-capable driver, and the user-installed runtime for NVIDIA Acceleration',
        ],
        'scope': 'Payload identity and static dependency audits only; Windows loading, GPU execution, '
        'and dynamically loaded dependencies still require native tests.',
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('bundle', type=Path)
    a = p.parse_args()
    print(json.dumps(audit(a.bundle.resolve()), indent=2))


if __name__ == '__main__':
    main()
