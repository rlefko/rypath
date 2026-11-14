#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare indexed PNG sources and selected U256 mip chains with pinned PBRT."""

import argparse
import hashlib
import io
import struct
import subprocess
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path

from compare_pbrt_image16 import (
    ENCODINGS, PNG_SIGNATURE, mip_dimensions, parse_mips, png_chunk, require_equal,
)
from extract_pbrt_rgb import REVISION


EXTERNAL_ASSETS = {
    "s35n3p04.png": "cd99fc4cc69bda1ff8e8697d8b423ae8d9e84a58ba1d2b480751953b6869e3b3",
    "tp1n3p08.png": "50bc927fe6cf1816454a7c27f8c743734be8db45e37e5ee3ede540f4e895e6c5",
}
WRAPS = ("repeat", "clamp")


@dataclass(frozen=True)
class Case:
    name: str
    dimensions: tuple[int, int]
    indexed: bytes
    expected_rgb: bytes
    compare_pbrt_mips: bool = False


def png(width, height, depth, color_type, rows, chunks=()):
    header = struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, 0)
    return (
        PNG_SIGNATURE + png_chunk(b"IHDR", header)
        + b"".join(png_chunk(name, payload) for name, payload in chunks)
        + png_chunk(b"IDAT", zlib.compress(rows)) + png_chunk(b"IEND", b"")
    )


def indexed_png(width, height, depth, palette, indices, alpha=b"", metadata=()):
    if (depth not in (1, 2, 4, 8) or not 0 < len(palette) <= 1 << depth
            or any(len(entry) != 3 for entry in palette)):
        raise ValueError("Invalid indexed PNG depth or palette size")
    if len(alpha) > len(palette):
        raise ValueError("Transparency entries exceed the palette size")
    if (min(width, height) < 1 or len(indices) != width * height
            or any(not 0 <= index < len(palette) for index in indices)):
        raise ValueError("Invalid indexed PNG dimensions or indices")

    padding_bits = (-width * depth) % 8
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # PNG filter None
        # Pack MSB-first. Set unused low bits so misreading padding cannot
        # happen to yield the first palette entry.
        packed = 0
        for x, index in enumerate(indices[y * width:(y + 1) * width]):
            used_bits = (x * depth) % 8
            shift = 8 - used_bits - depth
            packed |= index << shift
            if shift == 0:
                rows.append(packed)
                packed = 0
        if padding_bits:
            rows.append(packed | ((1 << padding_bits) - 1))
    chunks = [*metadata, (b"PLTE", b"".join(bytes(entry) for entry in palette))]
    if alpha:
        chunks.append((b"tRNS", alpha))
    return png(width, height, depth, 3, rows, chunks)


def rgb8_png(width, height, pixels):
    stride = width * 3
    if min(width, height) < 1 or len(pixels) != stride * height:
        raise ValueError("RGB8 byte count does not match dimensions")
    rows = b"".join(b"\0" + pixels[y:y + stride] for y in range(0, len(pixels), stride))
    return png(width, height, 8, 2, rows)


def generated_case(name, width, height, depth, palette, indices, alpha=b"", metadata=(),
                   compare_pbrt_mips=False):
    indexed = indexed_png(width, height, depth, palette, indices, alpha, metadata)
    rgb = b"".join(bytes(palette[index]) for index in indices)
    return Case(name, (width, height), indexed, rgb, compare_pbrt_mips)


def synthetic_cases():
    palette = tuple((i, 255 - i, (73 * i + 29) % 256) for i in range(256))
    alpha = bytes((0, 61))
    for depth, width, height, count in ((1, 9, 3, 2), (2, 5, 3, 3), (4, 7, 5, 13), (8, 5, 3, 11)):
        # A sub-byte row boundary changes the logical bit offset of the next row.
        # Keep zero-alpha RGB nonblack and cover omitted and partial alpha entries.
        indices = [(x + 3 * y) % count for y in range(height) for x in range(width)]
        yield generated_case(
            f"odd-{depth}bit", width, height, depth, palette[:count], indices,
            alpha[:1] if depth == 1 else alpha,
            metadata=((b"gAMA", struct.pack(">I", 100000)),) if depth == 4 else (),
        )

    # These permutations cover every palette byte code independently in R, G, and B.
    yield generated_case("all-codes", 16, 16, 8, palette, range(256), alpha)

    # Keep selected cross-renderer reductions away from ambiguous quantization
    # boundaries. These opposing colors also distinguish RGB and alpha handling.
    mip_palette = ((240, 40, 90), (24, 180, 216))
    mip_indices = (0, 0, 1, 0, 1, 1, 0, 1)
    for depth in (1, 2, 4, 8):
        yield generated_case(f"pot-{depth}bit", 4, 2, depth, mip_palette, mip_indices,
                             alpha[:1], compare_pbrt_mips=True)


def external_case(path, expected_hash):
    indexed = path.read_bytes()
    digest = hashlib.sha256(indexed).hexdigest()
    if digest != expected_hash:
        raise ValueError(f"{path}: expected SHA-256 {expected_hash}, got {digest}")
    try:
        from PIL import Image
    except ImportError as error:
        raise ValueError("Pillow is required to independently decode pinned PNGSuite assets") from error
    with Image.open(io.BytesIO(indexed)) as image:
        if image.format != "PNG" or image.mode != "P":
            raise ValueError(f"{path}: expected an indexed PNG")
        return Case(path.stem, image.size, indexed, image.convert("RGB").tobytes())


def parse_u256(output, dimensions, label):
    levels = parse_mips(output, dimensions, label)
    for level, codes in enumerate(levels):
        if any(code > 255 for code in codes):
            raise ValueError(f"{label}: mip {level} contains a code larger than 00ff")
    return levels


def read_probe(probe, mode, path, encoding, wrap, dimensions, label, output_directory):
    output = subprocess.check_output([probe, encoding, wrap, path, mode], text=True, timeout=30)
    levels = parse_u256(output, dimensions, label)
    if output_directory:
        (output_directory / f"{label}.txt").write_text(output, encoding="ascii")
    return levels


def require_equal_mips(expected, observed, dimensions, label):
    for level, (ref, actual, (width, _)) in enumerate(zip(
        expected, observed, dimensions, strict=True
    )):
        require_equal(ref, actual, width, f"{label} mip {level}")
    return sum(len(level) for level in expected)


def compare_case(case, probes, directory, output_directory):
    width, height = case.dimensions
    indexed = directory / f"{case.name}-indexed.png"
    indexed.write_bytes(case.indexed)
    rgb = directory / f"{case.name}-rgb.png"
    rgb.write_bytes(rgb8_png(width, height, case.expected_rgb))
    paths = {"indexed": indexed, "rgb": rgb}
    dimensions = list(mip_dimensions(width, height))

    def read(backend, kind, mode, encoding, wrap, sizes):
        label = f"{case.name}-{kind}-{backend}-{mode}-{encoding}-{wrap}"
        return read_probe(probes[backend], mode, paths[kind], encoding, wrap, sizes, label,
                          output_directory)

    source_codes = 0
    equivalent_codes = 0
    pinned_codes = 0
    for backend in probes:
        for kind in paths:
            source, = read(backend, kind, "source-u256", "linear", "repeat", [case.dimensions])
            require_equal(case.expected_rgb, source, width, f"{case.name} {backend} {kind} source")
            source_codes += len(source)
    for encoding in ENCODINGS:
        for wrap in WRAPS:
            mips = {}
            for backend in probes:
                expanded = read(backend, "rgb", "u256", encoding, wrap, dimensions)
                mips[backend] = read(backend, "indexed", "u256", encoding, wrap, dimensions)
                label = f"{case.name} {backend} {encoding} {wrap} RGB expansion"
                equivalent_codes += require_equal_mips(expanded, mips[backend], dimensions, label)
            if case.compare_pbrt_mips:
                label = f"{case.name} {encoding} {wrap} pinned PBRT"
                pinned_codes += require_equal_mips(mips["pbrt"], mips["rypath"], dimensions, label)
    return source_codes, equivalent_codes, pinned_codes


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
    modified = subprocess.check_output(
        [*git, "status", "--porcelain", "--ignore-submodules=none", "--untracked-files=all"],
        text=True,
    )
    if revision != REVISION or modified:
        raise ValueError(f"Expected a clean PBRT checkout at {REVISION}")
    probes = {
        "pbrt": args.pbrt_probe.resolve(strict=True),
        "rypath": args.rypath_probe.resolve(strict=True),
    }
    if args.output_directory:
        args.output_directory.mkdir(parents=True, exist_ok=True)

    cases = [*synthetic_cases(), *(external_case(args.image_assets / name, digest)
                                  for name, digest in EXTERNAL_ASSETS.items())]
    counts = []
    with tempfile.TemporaryDirectory(prefix="pbrt-indexed-") as directory:
        for case in cases:
            source, equivalent, pinned = compare_case(
                case, probes, Path(directory), args.output_directory
            )
            counts.append((source, equivalent, pinned))
            print(f"{case.name}: {source:,} source, {equivalent:,} within-backend mip, "
                  f"{pinned:,} cross-backend mip RGB codes")
    source, equivalent, pinned = map(sum, zip(*counts, strict=True))
    print(f"PBRT {REVISION}: all {len(cases)} cases passed "
          f"({source:,} source, {equivalent:,} within-backend mip, {pinned:,} cross-backend mip "
          "RGB codes checked exactly)")


if __name__ == "__main__":
    main()
