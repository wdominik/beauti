# SPDX-License-Identifier: GPL-3.0-or-later
"""Play a frame sequence through beauti-bench and report tracking and flicker metrics.

The same input gives comparable results on every machine and needs neither a camera nor
OBS. Each frame is analyzed in order with capture times of the clip, as if analysis always
kept up. Sources: a video file
(--clip), or a still image set in motion by a named synthetic scenario (--image and
--scenario). Clips of real people stay outside the repository.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

import cv2
import numpy as np
from clip_metrics import ClipMetrics

from beauti_tools.bench.protocol import pack_request, read_response

ANALYSIS_LONG_SIDE = 1280
SIZE = (1280, 720)
FRAMES = 120
SCENARIOS = ('static', 'pan', 'zoom_in', 'zoom_out', 'roll', 'edge', 'occlude', 'out_and_back', 'light')


def load_image(path, width=None, height=None):
    """An RGBA image of SIZE, cropped to fill; .rgba files are raw and need their size."""
    if path.suffix == '.rgba':
        image = np.fromfile(path, dtype=np.uint8).reshape(height, width, 4)[..., :3]
    else:
        read = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if read is None:
            raise ValueError(f'cannot read {path}')
        image = cv2.cvtColor(read, cv2.COLOR_BGR2RGB)
    scale = max(SIZE[0] / image.shape[1], SIZE[1] / image.shape[0])
    resized = cv2.resize(image, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
    left = (resized.shape[1] - SIZE[0]) // 2
    top = (resized.shape[0] - SIZE[1]) // 2
    return resized[top : top + SIZE[1], left : left + SIZE[0]]


def face_center(image, bundle, backend, cache):
    """Locate the face once, so that scenarios move it rather than the middle of the image."""
    frames = [cv2.cvtColor(image, cv2.COLOR_RGB2RGBA)]
    results = analyze(frames, bundle, backend, 'fast', 0, cache)
    if results[0][0].status != 0:
        raise ValueError('no face in the source image')
    left, top, right, bottom = results[0][0].box
    return (left + right) / 2, (top + bottom) / 2


def scenario_frames(image, name, center):
    """Yield RGBA frames. Sensor noise is seeded, so a scenario is the same on every machine."""
    rng = np.random.default_rng(1)
    width, height = SIZE
    for i in range(FRAMES):
        t = i / (FRAMES - 1)
        wave = np.sin(2 * np.pi * t)
        scale, angle, dx, dy, gain = 1.0, 0.0, 0.0, 0.0, 1.0
        match name:
            case 'pan':
                dx = 0.08 * width * wave
            case 'zoom_in':
                scale = 1 + t
            case 'zoom_out':
                scale = 1 - 0.8 * t
            case 'roll':
                angle = 40 * wave
            case 'edge':
                dx = (width * 0.95 - center[0]) * t
            case 'out_and_back':
                dx = 1.2 * width * np.sin(np.pi * t)
            case 'light':
                gain = 0.35 if 40 <= i < 80 else 1.0
        matrix = cv2.getRotationMatrix2D(center, angle, scale)
        matrix[:, 2] += (dx, dy)
        frame = cv2.warpAffine(image, matrix, SIZE, flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        if name == 'occlude' and 40 <= i < 60:
            half = int(0.2 * height)
            cx, cy = int(center[0]), int(center[1])
            frame[max(0, cy - half) : cy + half, max(0, cx - half) : cx + half] = (70, 60, 55)
        noisy = frame * gain + rng.normal(0, 1.5, frame.shape)
        yield cv2.cvtColor(np.clip(noisy, 0, 255).astype(np.uint8), cv2.COLOR_RGB2RGBA)


def clip_frames(path, fps=None):
    """Yield RGBA frames of a video, scaled like the plugin's analysis image."""
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise ValueError(f'cannot open {path}')
    source_fps = capture.get(cv2.CAP_PROP_FPS) or 30
    step = max(1.0, source_fps / fps) if fps else 1.0
    index, due = 0, 0.0
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        if index + 1e-6 >= due:
            due += step
            scale = min(1.0, ANALYSIS_LONG_SIDE / max(frame.shape[:2]))
            if scale < 1:
                frame = cv2.resize(frame, None, fx=scale, fy=scale, interpolation=cv2.INTER_LINEAR)
            yield cv2.cvtColor(frame, cv2.COLOR_BGR2RGBA)
        index += 1
    capture.release()


def worker_command(bundle, backend, quality, eyes, cache, frame_ms):
    darwin = sys.platform == 'darwin'
    return [
        str(bundle / ('Contents/Resources/bin/beauti-bench' if darwin else 'beauti-bench.exe')),
        '--models',
        str(bundle / ('Contents/Resources/models' if darwin else 'data/models')),
        '--backend',
        backend,
        '--quality',
        quality,
        '--eyes',
        str(eyes),
        '--frame-ms',
        str(frame_ms),
        '--cache',
        str(cache),
    ]


def analyze(frames, bundle, backend, quality, eyes, cache, on_result=None, frame_ms=1000 / 30):
    """Run frames through one worker; returns (response, frame) pairs unless on_result takes them."""
    command = worker_command(bundle, backend, quality, eyes, cache, frame_ms)
    results = []
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as worker:
        try:
            for sequence, frame in enumerate(frames, 1):
                height, width = frame.shape[:2]
                worker.stdin.write(pack_request(width, height, 100, 100, sequence, frame.tobytes()))
                worker.stdin.flush()
                response = read_response(worker.stdout)
                if on_result:
                    on_result(response, frame)
                else:
                    results.append((response, frame))
        finally:
            worker.stdin.close()
    if worker.returncode:
        raise RuntimeError(f'beauti-bench failed with exit code {worker.returncode}')
    return results


def measure(frames, bundle, backend, quality, eyes, cache, frame_ms=1000 / 30):
    """Return the summary for a frame source."""
    metrics = None

    def collect(response, frame):
        nonlocal metrics
        metrics = metrics or ClipMetrics(frame.shape[1], frame.shape[0])
        side = response.mask_size
        mask = np.frombuffer(response.mask, np.float32).reshape(side, side, 4) if side else None
        side = response.blend_size
        blend = np.frombuffer(response.blend, np.float32).reshape(side, side, 4) if side else None
        metrics.add(response.status, response.box, mask, blend, frame, response.milliseconds)

    analyze(frames, bundle, backend, quality, eyes, cache, collect, frame_ms)
    if metrics is None:
        raise ValueError('the source has no frames')
    return metrics.summary()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bundle', type=Path, required=True)
    p.add_argument('--backend', choices=['cpu', 'coreai', 'tensorrt', 'tensorrt-fp32'], required=True)
    p.add_argument('--quality', choices=['fast', 'full'], default='fast')
    p.add_argument('--eyes', type=int, default=100)
    p.add_argument('--output', type=Path, required=True, help='JSON report')
    p.add_argument('--cache', type=Path, required=True, help='Provider cache directory')
    p.add_argument('--clip', type=Path, nargs='*', default=[], help='Video files')
    p.add_argument('--fps', type=float, help='Analyze a video at this rate')
    p.add_argument('--image', type=Path, nargs='*', default=[], help='Still images for the synthetic scenarios')
    p.add_argument('--width', type=int, help='Width of a .rgba image')
    p.add_argument('--height', type=int, help='Height of a .rgba image')
    p.add_argument('--scenario', nargs='+', choices=SCENARIOS, default=list(SCENARIOS))
    a = p.parse_args()
    if not a.clip and not a.image:
        p.error('Give --clip or --image')
    report = {'backend': a.backend, 'quality': a.quality, 'eyes': a.eyes, 'runs': {}}
    a.output.parent.mkdir(parents=True, exist_ok=True)

    def record(name, frames):
        summary = measure(frames, a.bundle, a.backend, a.quality, a.eyes, a.cache, 1000 / (a.fps or 30))
        report['runs'][name] = summary
        a.output.write_text(json.dumps(report, indent=2) + '\n')
        print(name, json.dumps(summary), flush=True)

    for clip in a.clip:
        record(clip.name, clip_frames(clip, a.fps))
    for path in a.image:
        image = load_image(path, a.width, a.height)
        center = face_center(image, a.bundle, a.backend, a.cache)
        for scenario in a.scenario:
            record(f'{path.stem}/{scenario}', scenario_frames(image, scenario, center))


if __name__ == '__main__':
    main()
