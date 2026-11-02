# SPDX-License-Identifier: GPL-3.0-or-later
"""Binary protocol of beauti-bench (tools/benchmarks/worker.cpp). Keep both in sync.

Standard library only: beauti-verify-native runs right after a build, before any
test packages are installed. Payloads are returned as raw little-endian float32
bytes; tests convert them with numpy.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import BinaryIO

MAGIC = 0x324C4B53
# magic, width, height, retouch, smooth, sequence; followed by width*height*4 RGBA bytes
REQUEST = struct.Struct('<6I')
# magic, width, height, status, sequence, used_backend, blend_size, mask_size, raw_mask;
# box left, top, right, bottom; total, detect, infer, post milliseconds
RESPONSE = struct.Struct('<9I4i4f')
# beauti::Backend values (src/backends/backend.h)
BACKEND_IDS = {'coreai': 0, 'cpu': 2, 'tensorrt': 3, 'tensorrt-fp32': 4}
FLOATS_PER_TEXEL = 4


@dataclass(frozen=True)
class Response:
    width: int
    height: int
    status: int
    sequence: int
    used_backend: int
    blend_size: int
    mask_size: int
    raw_mask: bool
    box: tuple[int, int, int, int]
    milliseconds: float
    detect_ms: float
    infer_ms: float
    post_ms: float
    blend: bytes  # blend_size * blend_size * 4 float32, RGBA interleaved
    mask: bytes  # mask_size * mask_size * 4 float32, RGBA interleaved

    @property
    def has_face(self) -> bool:
        return self.status == 0


def pack_request(width: int, height: int, retouch: int, smooth: int, sequence: int, pixels: bytes) -> bytes:
    if len(pixels) != width * height * 4:
        raise ValueError('pixel buffer does not match width*height*4')
    return REQUEST.pack(MAGIC, width, height, retouch, smooth, sequence) + pixels


def parse_responses(data: bytes) -> list[Response]:
    """Parse the complete stdout of a worker run; raises on any framing error."""
    responses = []
    offset = 0
    while offset < len(data):
        if len(data) - offset < RESPONSE.size:
            raise ValueError(f'truncated response header at offset {offset}')
        fields = RESPONSE.unpack_from(data, offset)
        offset += RESPONSE.size
        if fields[0] != MAGIC:
            raise ValueError(f'bad magic at offset {offset - RESPONSE.size}: {fields[0]:#x}')
        blend_bytes = fields[6] * fields[6] * FLOATS_PER_TEXEL * 4
        mask_bytes = fields[7] * fields[7] * FLOATS_PER_TEXEL * 4
        if len(data) - offset < blend_bytes + mask_bytes:
            raise ValueError(f'truncated payload for frame {fields[4]}')
        blend = data[offset : offset + blend_bytes]
        offset += blend_bytes
        mask = data[offset : offset + mask_bytes]
        offset += mask_bytes
        responses.append(
            Response(
                width=fields[1],
                height=fields[2],
                status=fields[3],
                sequence=fields[4],
                used_backend=fields[5],
                blend_size=fields[6],
                mask_size=fields[7],
                raw_mask=bool(fields[8]),
                box=tuple(fields[9:13]),
                milliseconds=fields[13],
                detect_ms=fields[14],
                infer_ms=fields[15],
                post_ms=fields[16],
                blend=blend,
                mask=mask,
            )
        )
    return responses


def read_response(stream: BinaryIO) -> Response:
    """Read one response from the worker's stdout, for clips too long to hold in memory."""
    header = stream.read(RESPONSE.size)
    if len(header) != RESPONSE.size:
        raise ValueError('truncated response header')
    fields = RESPONSE.unpack(header)
    if fields[0] != MAGIC:
        raise ValueError(f'bad magic: {fields[0]:#x}')
    payload = stream.read((fields[6] * fields[6] + fields[7] * fields[7]) * FLOATS_PER_TEXEL * 4)
    return parse_responses(header + payload)[0]
