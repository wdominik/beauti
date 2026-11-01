# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage byte-identical Apache-2.0 FaceMesh V2 and Iris ONNX weights for CPU inference."""

import hashlib
import io
import json
import tarfile

from ..paths import MODELS, ROOT


def member(archive: tarfile.TarFile, name: str) -> bytes:
    """Read a named regular member without extracting archive paths to disk."""
    info = archive.getmember(name)
    if not info.isfile() or info.size > 100_000_000:
        raise ValueError('Invalid eye model archive member: ' + name)
    stream = archive.extractfile(info)
    if stream is None:
        raise ValueError('Missing eye model archive member: ' + name)
    with stream:
        return stream.read()


def main():
    manifest = json.loads((ROOT / 'models/manifest.json').read_text())['files']
    sources = json.loads((ROOT / 'models/sources.json').read_text())['files']
    for name in ('face_landmarker.onnx', 'iris-resources.tar.gz'):
        with (MODELS / name).open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != sources['.cache/models/' + name]['sha256']:
            raise ValueError('Eye model source checksum mismatch: ' + name)
    with tarfile.open(MODELS / 'iris-resources.tar.gz', 'r:gz') as outer:
        inner_bytes = member(outer, '20_new_20211209/resources.tar.gz')
    with tarfile.open(fileobj=io.BytesIO(inner_bytes), mode='r:gz') as inner:
        iris = member(inner, 'saved_model_64x64/model_float32.onnx')
    files = {'face_landmarks.onnx': (MODELS / 'face_landmarker.onnx').read_bytes(), 'iris_landmarks.onnx': iris}
    (MODELS / 'native').mkdir(exist_ok=True)
    for name, data in files.items():
        if hashlib.sha256(data).hexdigest() != manifest['native/' + name]:
            raise ValueError('Prepared eye model checksum mismatch: ' + name)
        (MODELS / 'native' / name).write_bytes(data)


if __name__ == '__main__':
    main()
