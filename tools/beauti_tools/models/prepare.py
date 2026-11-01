# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare the model graphs used by the OBS package, in dependency order."""

import argparse
import subprocess
import sys

from ..paths import ROOT


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        '--check',
        action='store_true',
        help='Verify existing inputs and prepared outputs without running transformations',
    )
    a = p.parse_args()
    subprocess.run(
        [sys.executable, '-m', 'beauti_tools.models.download', *(['--check'] if a.check else [])], check=True
    )
    if not a.check:
        for name in [
            'export_model',
            'prepare_models',
            'prepare_fp16',
            'prepare_native_models',
            'prepare_eye_models',
            'prepare_mediapipe',
            # Core AI assets need macOS; other hosts keep the recorded ones.
            *(['prepare_coreai'] if sys.platform == 'darwin' else []),
        ]:
            subprocess.run([sys.executable, '-m', 'beauti_tools.models.' + name], check=True, cwd=ROOT)
    subprocess.run([sys.executable, '-m', 'beauti_tools.verify_provenance', '--models'], check=True)


if __name__ == '__main__':
    main()
