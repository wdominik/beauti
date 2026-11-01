# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the standalone CPU reference effects; no OBS or ONNX SDK needed."""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from ..paths import ROOT


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    local_cmake = Path(sys.executable).with_name('cmake.exe' if sys.platform == 'win32' else 'cmake')
    parser.add_argument(
        '--cmake', default=str(local_cmake) if local_cmake.is_file() else shutil.which('cmake') or 'cmake'
    )
    args = parser.parse_args()
    build = ROOT / 'build' / 'effects'
    subprocess.run(
        [
            args.cmake,
            '-S',
            str(ROOT / 'src/effects'),
            '-B',
            str(build),
            '-DCMAKE_BUILD_TYPE=Release',
            '-DBUILD_TESTING=OFF',
        ],
        check=True,
    )
    subprocess.run([args.cmake, '--build', str(build), '--config', 'Release', '--parallel'], check=True)
    print('Beauti CPU reference effects are ready.')


if __name__ == '__main__':
    main()
