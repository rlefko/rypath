#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare low-bit grayscale PNG sources and selected U256 mips with pinned PBRT."""

import argparse
import hashlib
import io
import struct
import subprocess
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path

from compare_pbrt_image16 import ENCODINGS, PNG_SIGNATURE, mip_dimensions, png_chunk, require_equal
from compare_pbrt_indexed import read_probe, require_equal_mips, rgb8_png
from extract_pbrt_rgb import REVISION


ASSET_NAME = "tbbn0g04.png"
ASSET_SHA256 = "c5e7eeaffa677bfac002ee968a0423e0fea3be7701d39ccd6a015df1d2d85e40"
EXPANSION = {1: 255, 2: 85, 4: 17}
WRAPS = ("repeat", "clamp")


@dataclass(frozen=True)
class Case:
    name: str
    dimensions: tuple[int, int]
    grayscale: bytes
    expected_rgb: bytes
    compare_pbrt_mips: bool = False


def generated_case(name, width, height, depth, samples, key=None, split_idat=False,
                   compare_pbrt_mips=False):
    padding = (-width * depth) % 8
    row_bytes = (width * depth + padding) // 8
    rows = bytearray()
    for y in range(height):
        packed = 0
        for sample in samples[y * width:(y + 1) * width]:
            packed = (packed << depth) | sample
        # Fill unused low bits with ones so reading across rows would change samples.
        packed = (packed << padding) | ((1 << padding) - 1)
        rows.append(0)  # PNG filter None
        rows.extend(packed.to_bytes(row_bytes, "big"))
    chunks = [
        (b"IHDR", struct.pack(">IIBBBBB", width, height, depth, 0, 0, 0, 0)),
        (b"gAMA", struct.pack(">I", 100000)),
    ]
    if key is not None:
        chunks.append((b"tRNS", struct.pack(">H", key)))
    compressed = zlib.compress(rows)
    blocks = (compressed[:1], compressed[1:-2], compressed[-2:]) if split_idat else (compressed,)
    chunks.extend((b"IDAT", block) for block in blocks)
    chunks.append((b"IEND", b""))
    grayscale = PNG_SIGNATURE + b"".join(png_chunk(tag, payload) for tag, payload in chunks)
    expected_rgb = b"".join(bytes([sample * EXPANSION[depth]]) * 3 for sample in samples)
    return Case(name, (width, height), grayscale, expected_rgb, compare_pbrt_mips)


def synthetic_cases():
    for depth, width in ((1, 9), (2, 5), (4, 7)):
        maximum = (1 << depth) - 1
        samples = [(x + width * y) & maximum for y in range(5) for x in range(width)]
        yield generated_case(f"odd-{depth}bit", width, 5, depth, samples, key=maximum,
                             split_idat=depth == 2)
        # Reductions of one white texel among seven black texels avoid ambiguous
        # cross-renderer quantization boundaries in both encodings.
        yield generated_case(f"pot-{depth}bit", 4, 2, depth, (0, 0, maximum, 0, 0, 0, 0, 0),
                             compare_pbrt_mips=True)


def external_case(path):
    grayscale = path.read_bytes()
    digest = hashlib.sha256(grayscale).hexdigest()
    if digest != ASSET_SHA256:
        raise ValueError(f"{path}: expected SHA-256 {ASSET_SHA256}, got {digest}")
    try:
        from PIL import Image
    except ImportError as error:
        raise ValueError("Pillow is required to independently decode the pinned PNGSuite asset") from error
    with Image.open(io.BytesIO(grayscale)) as image:
        # RGB conversion exposes the stored colors without applying the transparency key.
        return Case(path.stem, image.size, grayscale, image.convert("RGB").tobytes())


def compare_case(case, probes, directory, output_directory):
    paths = {kind: directory / f"{case.name}-{kind}.png" for kind in ("gray", "rgb")}
    paths["gray"].write_bytes(case.grayscale)
    paths["rgb"].write_bytes(rgb8_png(*case.dimensions, case.expected_rgb))
    dimensions = list(mip_dimensions(*case.dimensions))

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
            require_equal(case.expected_rgb, source, case.dimensions[0],
                          f"{case.name} {backend} {kind} source")
            source_codes += len(source)
    for encoding in ENCODINGS:
        for wrap in WRAPS:
            mips = {}
            for backend in probes:
                expanded = read(backend, "rgb", "u256", encoding, wrap, dimensions)
                mips[backend] = read(backend, "gray", "u256", encoding, wrap, dimensions)
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

    cases = [*synthetic_cases(), external_case(args.image_assets / ASSET_NAME)]
    counts = []
    with tempfile.TemporaryDirectory(prefix="pbrt-grayscale-") as directory:
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
