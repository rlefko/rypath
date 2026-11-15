#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate the owned small PNGs used by the core texture comparisons."""

from pathlib import Path
import struct
import zlib


def chunk(name, payload):
    return (struct.pack(">I", len(payload)) + name + payload
            + struct.pack(">I", zlib.crc32(name + payload)))


def png(width, height, color_type, depth, rows):
    data = b"".join(b"\0" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(data))
            + chunk(b"IEND", b""))


def main():
    directory = Path(__file__).resolve().parent / "assets"
    directory.mkdir(exist_ok=True)
    images = {
        "white.png": png(1, 1, 0, 8, [bytes([255])]),
        "zero.png": png(1, 1, 0, 8, [bytes([0])]),
        "gray-pattern.png": png(4, 4, 0, 8, [
            bytes(row) for row in [[10, 125, 250, 80], [90, 80, 65, 170],
                                   [255, 200, 20, 40], [170, 220, 80, 100]]
        ]),
        "normal-flat.png": png(1, 1, 2, 16, [struct.pack(">HHH", 32768, 32768, 65535)]),
        "normal-tilted.png": png(1, 1, 2, 16, [struct.pack(">HHH", 22000, 32768, 62000)]),
        "emission.png": png(2, 2, 2, 8, [
            bytes([210, 84, 42, 130, 235, 65]), bytes([32, 110, 210, 190, 230, 160])
        ]),
        "environment.png": png(4, 4, 2, 8, [
            bytes(channel for x in range(4)
                  for channel in (64 + 36 * x, 67 + 38 * y, 190 if (x + y) % 2 else 135))
            for y in range(4)
        ]),
    }
    for name, payload in images.items():
        (directory / name).write_bytes(payload)


if __name__ == "__main__":
    main()
