# SPDX-License-Identifier: GPL-3.0-or-later
"""Render README before/after images through the built plugin in headless OBS.

Each input image is scaled to at most 3840 px on its long side, processed once by
beauti-obs-check with a look preset, and written as a full frame and a face crop,
each in an original and a processed version. Needs the dev dependency group, a
staged bundle, and the native build (docs/TESTING.md).
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np

from .paths import ROOT

WINDOWS = sys.platform == 'win32'
# Look preset IDs (src/core/presets.h).
PRESETS = {'low': 101, 'medium': 102, 'high': 103, 'very-high': 104}
LONG_SIDE = 3840
FRAME_WIDTH, CROP_SIDE, JPEG_QUALITY = 1600, 800, 90
# The runner draws its frame counter into the top 16 rows of the source; the image is
# placed below them and the band is cut off again.
COUNTER_ROWS = 16


def scaled(image):
    """Scale to at most LONG_SIDE on the long side, with even dimensions."""
    h, w = image.shape[:2]
    s = min(1.0, LONG_SIDE / max(w, h))
    size = (int(w * s) // 2 * 2, int(h * s) // 2 * 2)
    return cv2.resize(image, size, interpolation=cv2.INTER_AREA) if size != (w, h) else image


def render(image, preset, quality, args, work):
    """One headless OBS run on a static frame; returns the last output frame (BGR)."""
    padded = cv2.copyMakeBorder(image, COUNTER_ROWS, 0, 0, 0, cv2.BORDER_CONSTANT, value=(0, 0, 0))
    h, w = padded.shape[:2]
    raw = work / 'input.rgba'
    cv2.cvtColor(padded, cv2.COLOR_BGR2RGBA).tofile(raw)
    settings = work / 'settings.json'
    settings.write_text(json.dumps({'preset': PRESETS[preset]}))
    report = work / 'report.json'
    command = [args.harness, args.bundle, raw, report, args.seconds, quality, 'default', 0, 0, w, h, 0, 0, 0]
    command = [str(c) for c in command] + ['en-US', 'Beauti']
    env = os.environ.copy()
    for key in ('BEAUTI_TIMING', 'BEAUTI_ORT_PROFILE_DIR', 'BEAUTI_FIXTURE_FPS', 'BEAUTI_FIXTURE_CUT'):
        env.pop(key, None)
    env['BEAUTI_OBS_RUNTIME'] = str(args.obs_runtime)
    env['BEAUTI_FIXTURE_SETTINGS'] = str(settings)
    if WINDOWS:
        env['PATH'] = str(args.obs_runtime / 'bin/64bit') + ';' + os.environ['SYSTEMROOT'] + '/System32'
    log = work / 'obs.log'
    with log.open('w') as stream:
        subprocess.run(command, env=env, stdout=stream, stderr=stream, check=True, timeout=300)
    samples = json.loads(report.read_text())['samples']
    # Sample columns: time, source frame, output frame, difference to the source, white share.
    if not samples or samples[-1][3] <= 0:
        raise SystemExit(f'The last frame was not processed (no face found?); see {log}')
    bgra = np.fromfile(report.with_suffix('.json.rgba'), np.uint8).reshape(h, w, 4)
    return bgra[COUNTER_ROWS:, :, :3].copy()


def face_box(original, processed):
    """Square around the clearly changed pixels, which the effect limits to facial skin."""
    changed = np.abs(original.astype(int) - processed.astype(int)).max(axis=2) > 12
    ys, xs = np.nonzero(changed)
    if len(xs) < 100:
        raise SystemExit('The processed frame barely differs from the original')
    x0, x1 = np.percentile(xs, [2, 98])
    y0, y1 = np.percentile(ys, [2, 98])
    side = int(max(x1 - x0, y1 - y0) * 1.5)
    h, w = original.shape[:2]
    side = min(side, w, h)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    left = int(np.clip(cx - side / 2, 0, w - side))
    top = int(np.clip(cy - side / 2, 0, h - side))
    return left, top, side


def write(path, image):
    cv2.imwrite(str(path), image, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY, cv2.IMWRITE_JPEG_OPTIMIZE, 1])
    print(f'{path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}: {path.stat().st_size // 1024} KB')


def main():
    windows_bundle = ROOT / 'dist/tensorrt/beauti'
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('images', type=Path, nargs='+')
    p.add_argument('--names', nargs='+', help='Output names, one per image (default: the file names)')
    p.add_argument('--preset', choices=sorted(PRESETS), default='high')
    p.add_argument('--quality', choices=['fast', 'full'], default='full')
    p.add_argument('--output', type=Path, default=ROOT / 'docs/images')
    p.add_argument('--bundle', type=Path, default=windows_bundle if WINDOWS else ROOT / 'dist/beauti.plugin')
    p.add_argument(
        '--harness',
        type=Path,
        default=ROOT
        / ('build/windows-tensorrt/Release/beauti-obs-check.exe' if WINDOWS else 'build/macos/beauti-obs-check'),
    )
    p.add_argument(
        '--obs-runtime',
        type=Path,
        default=None if WINDOWS else Path('/Applications/OBS.app/Contents/Frameworks'),
        help='OBS runtime directory (required on Windows)',
    )
    p.add_argument('--seconds', type=float, default=8, help='Run time per image; the last frame is kept')
    a = p.parse_args()
    if a.obs_runtime is None:
        p.error('--obs-runtime is required on Windows')
    names = a.names or [image.stem for image in a.images]
    if len(names) != len(a.images):
        p.error('--names needs one name per image')
    a.output.mkdir(parents=True, exist_ok=True)
    for image_path, name in zip(a.images, names, strict=True):
        image = cv2.imread(str(image_path), cv2.IMREAD_COLOR)
        if image is None:
            raise SystemExit(f'Cannot read {image_path}')
        original = scaled(image)
        with tempfile.TemporaryDirectory() as work:
            processed = render(original, a.preset, 1 if a.quality == 'full' else 0, a, Path(work))
        left, top, side = face_box(original, processed)
        frame_size = (FRAME_WIDTH, round(original.shape[0] * FRAME_WIDTH / original.shape[1]))
        for label, picture in (('original', original), (a.preset, processed)):
            write(a.output / f'{name}-{label}.jpg', cv2.resize(picture, frame_size, interpolation=cv2.INTER_AREA))
            crop = picture[top : top + side, left : left + side]
            write(
                a.output / f'{name}-face-{label}.jpg',
                cv2.resize(crop, (CROP_SIDE, CROP_SIDE), interpolation=cv2.INTER_AREA),
            )


if __name__ == '__main__':
    main()
