#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare the stored RGB binary16 mip chains with a separately linked pinned PBRT."""

import argparse
import hashlib
import math
import re
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

from extract_pbrt_rgb import REVISION, f32


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
AUTHORED_PIXELS = (
    (0x0101, 0x03F8, 0x0BF2), (0x01FE, 0x03FE, 0x0B02),
    (0x0132, 0x03BA, 0x0BF8), (0x01CD, 0x0312, 0x0BB9),
    (0x01E9, 0x0336, 0x0B4B), (0x0120, 0x0399, 0x0BD9),
    (0x01BA, 0x0309, 0x0B23), (0x0154, 0x03CD, 0x0B70),
)
REDUCTION_CODES = (1, 1, 255, 0x1235)
ENCODINGS = ("linear", "sRGB")
HALF_ROW = re.compile(r"[0-7][0-9a-f]{3}(?: [0-7][0-9a-f]{3})*")


def png_chunk(name, payload):
    return struct.pack(">I", len(payload)) + name + payload + struct.pack(
        ">I", zlib.crc32(name + payload)
    )


def png16(width, height, color_type, pixels):
    channels = {0: 1, 2: 3, 6: 4}[color_type]
    if len(pixels) != width * height or any(len(pixel) != channels for pixel in pixels):
        raise ValueError("Invalid fixture dimensions or channel count")
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # PNG filter None
        for pixel in pixels[y * width:(y + 1) * width]:
            rows.extend(struct.pack(f">{channels}H", *pixel))
    header = struct.pack(">IIBBBBB", width, height, 16, color_type, 0, 0, 0)
    return (
        PNG_SIGNATURE + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(rows, level=0)) + png_chunk(b"IEND", b"")
    )


def mip_dimensions(width, height):
    width, height = (1 << (size - 1).bit_length() for size in (width, height))
    while (width, height) != (1, 1):
        yield width, height
        width, height = max(1, width // 2), max(1, height // 2)
    yield 1, 1


def parse_mips(output, dimensions, label):
    lines = iter(output.splitlines())
    levels = []
    for level, (width, height) in enumerate(dimensions):
        if next(lines, None) != f"{width} {height}":
            raise ValueError(f"{label}: mip {level} must have dimensions {width}x{height}")
        values = []
        for row in range(height):
            line = next(lines, "")
            if len(line) != 15 * width - 1 or not HALF_ROW.fullmatch(line):
                raise ValueError(f"{label}: malformed mip {level} row {row}")
            codes = [int(word, 16) for word in line.split()]
            if any(code >= 0x7c00 for code in codes):
                raise ValueError(f"{label}: nonfinite mip {level} row {row}")
            values.extend(codes)
        levels.append(values)
    if next(lines, None) is not None:
        raise ValueError(f"{label}: unexpected data after the last mip")
    return levels


def require_equal(reference, actual, width, label):
    if len(reference) != len(actual):
        raise ValueError(f"{label}: expected {len(reference)} values, got {len(actual)}")
    for index, (expected, observed) in enumerate(zip(reference, actual, strict=True)):
        if expected != observed:
            y, x = divmod(index // 3, width)
            channel = "RGB"[index % 3]
            raise ValueError(
                f"{label}: ({x}, {y}) {channel} expected {expected:04x}, got {observed:04x}"
            )


def half_bits(value):
    return struct.unpack(">H", struct.pack(">e", value))[0]


def half_value(bits):
    return struct.unpack(">e", struct.pack(">H", bits))[0]


def linear_half(code):
    return half_bits(f32(code / 65535))


def reduction_expected():
    base = [linear_half(code) for code in REDUCTION_CODES]
    working = [half_value(bits) for bits in base]
    first = [(working[0] + working[1]) / 2, (working[2] + working[3]) / 2]
    return base, [half_bits(value) for value in first], [half_bits(sum(first) / 2)]


def resize_x2(taps):
    """Half code at x=2 after a 3-to-4 resize, given four already wrapped taps."""
    def sinc(offset):
        angle = math.pi * offset
        return math.sin(angle) / angle

    # The output center is (2 + 0.5) * 3/4; the contributing input centers
    # are 0.5, 1.5, 2.5, 3.5. The height is one, so only x contributes.
    offsets = (-1.375, -0.375, 0.625, 1.625)
    weights = [sinc(offset) * sinc(offset / 2) for offset in offsets]
    value = sum(tap * weight for tap, weight in zip(taps, weights, strict=True)) / sum(weights)
    return half_bits(max(0.0, value))


def compare_case(pbrt_probe, rypath_probe, path, encoding, wrap, output_directory):
    with path.open("rb") as image:
        header = image.read(29)
    if len(header) != 29 or header[:16] != PNG_SIGNATURE + b"\x00\x00\x00\x0dIHDR":
        raise ValueError(f"{path}: expected a PNG header")
    width, height, bit_depth = struct.unpack_from(">IIB", header, 16)
    if width < 1 or height < 1 or bit_depth != 16:
        raise ValueError(f"{path}: expected a 16-bit PNG")
    dimensions = list(mip_dimensions(width, height))
    name = f"{path.stem}-{encoding}-{wrap}"
    outputs = {}
    levels = {}
    for renderer, probe in (("pbrt", pbrt_probe), ("rypath", rypath_probe)):
        output = subprocess.check_output([probe, encoding, wrap, path], text=True, timeout=30)
        outputs[renderer] = output
        levels[renderer] = parse_mips(output, dimensions, f"{name} {renderer}")
    for level, (ref, actual, (mip_width, _)) in enumerate(zip(
        levels["pbrt"], levels["rypath"], dimensions, strict=True
    )):
        require_equal(ref, actual, mip_width, f"{name} mip {level}")
    if output_directory:
        for renderer, output in outputs.items():
            path = output_directory / f"{name}-{renderer}.txt"
            path.write_text(output, encoding="ascii")
            print(f"{path}: SHA-256 {hashlib.sha256(path.read_bytes()).hexdigest()}")
    values = sum(len(level) for level in levels["pbrt"])
    print(f"{name}: {values:,} stored RGB half codes match across {len(dimensions)} mips")
    return levels["pbrt"]


def validate(args, directory):
    def compare(path, encoding, wrap="repeat"):
        return compare_case(args.pbrt_probe, args.rypath_probe, path, encoding, wrap,
                            args.output_directory)

    codes = [(code, 65535 - code, (code * 40503 + 107) % 65536) for code in range(65536)]
    exhaustive = directory / "exhaustive.png"
    exhaustive.write_bytes(png16(256, 256, 2, codes))
    linear_lookup = [linear_half(code) for code in range(65536)]
    expected = [linear_lookup[channel] for pixel in codes for channel in pixel]
    linear = compare(exhaustive, "linear")
    require_equal(expected, linear[0], 256, "exhaustive independently rounded linear base")
    print(f"Independent linear conversion: {len(expected):,} binary16 codes match")
    compare(exhaustive, "sRGB")

    reduction = directory / "reduction.png"
    reduction.write_bytes(png16(4, 1, 0, [(code,) for code in REDUCTION_CODES]))
    for width, expected, observed in zip((4, 2, 1), reduction_expected(),
                                         compare(reduction, "linear"), strict=True):
        require_equal([bits for bits in expected for _ in range(3)], observed, width,
                      f"independent {width}x1 reduction")

    resize = directory / "resize.png"
    resize.write_bytes(png16(3, 1, 0, [(0,), (65535,), (65535,)]))
    for wrap, last_tap in (("clamp", 1), ("repeat", 0)):
        base, *_ = compare(resize, "linear", wrap)
        expected = resize_x2((0, 1, 1, last_tap))
        require_equal([expected] * 3, base[6:9], 1, f"{wrap} resize overshoot at (2, 0)")

    resize_rounding = directory / "resize-rounding.png"
    source = (1, 0x8888, 1)
    resize_rounding.write_bytes(png16(3, 1, 0, [(code,) for code in source]))
    base, *_ = compare(resize_rounding, "linear", "clamp")
    expected = resize_x2([half_value(linear_half(code)) for code in (*source, source[-1])])
    require_equal([expected] * 3, base[6:9], 1, "resize after source half rounding at (2, 0)")

    alpha = directory / "alpha.png"
    alpha.write_bytes(png16(4, 2, 6, [
        (*pixel, index * 65535 // 7) for index, pixel in enumerate(AUTHORED_PIXELS)
    ]))
    authored = args.image_assets / "asymmetric16.png"
    if authored.read_bytes() != png16(4, 2, 2, AUTHORED_PIXELS):
        raise ValueError(f"{authored}: differs from the authored fixture generator")
    for encoding in ENCODINGS:
        rgb = compare(authored, encoding)
        rgba = compare(alpha, encoding)
        if rgb != rgba:
            raise ValueError(f"{encoding}: alpha changed stored RGB")
        for filename in ("basn2c16.png", "basn0g16.png"):
            compare(args.image_assets / filename, encoding)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pbrt_source", type=Path)
    parser.add_argument("pbrt_probe", type=Path)
    parser.add_argument("rypath_probe", type=Path)
    parser.add_argument("image_assets", type=Path)
    parser.add_argument("--output-directory", type=Path)
    args = parser.parse_args()

    git = ["git", "-C", args.pbrt_source]
    revision = subprocess.check_output([*git, "rev-parse", "HEAD"], text=True).strip()
    modified = subprocess.check_output([*git, "status", "--porcelain"], text=True)
    if revision != REVISION or modified:
        raise ValueError(f"Expected a clean PBRT checkout at {REVISION}")
    args.pbrt_probe = args.pbrt_probe.resolve(strict=True)
    args.rypath_probe = args.rypath_probe.resolve(strict=True)
    if args.output_directory:
        args.output_directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pbrt-image16-") as directory:
        validate(args, Path(directory))
    print(f"PBRT {REVISION}: all binary16 mip and precision checks passed")


if __name__ == "__main__":
    main()
