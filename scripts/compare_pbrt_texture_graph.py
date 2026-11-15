#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare standalone binaries from the pinned PBRT and rypath texture graph probes.

The PBRT revision is b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f. Build its float/
Release pbrt_lib with PBRT_FLOAT_AS_DOUBLE=OFF. Link
scripts/pbrt_texture_graph_probe.cpp as C++17 with the include paths and link
dependencies used by that build's pbrt executable. Build
scripts/rypath_texture_graph_probe.cpp as C++20 with -Isrc -Ishaders and link
rypath_render, rypath_pbrt, rypath_rgb_spectrum, rypath_pbrt_spectra, rypath_image,
rypath_png, and rypath_jpeg from a rypath build. The probes receive the same synthetic files.
"""

import argparse
from collections import defaultdict
import math
from pathlib import Path
import struct
import subprocess
import zlib


def write_png(path, grayscale):
    """Write an 8x8 linear byte image whose color varies along both axes."""

    def chunk(name, data):
        return (
            struct.pack(">I", len(data))
            + name
            + data
            + struct.pack(">I", zlib.crc32(name + data))
        )

    header = struct.pack(">IIBBBBB", 8, 8, 8, 0 if grayscale else 2, 0, 0, 0)
    rows = bytearray()
    for y in range(8):
        rows.append(0)
        for x in range(8):
            rows.append((x * 23 + y * 57 + 68) & 255)
            if not grayscale:
                rows.extend(((x * 43 + y * 17 + 19) & 255, (x * 11 + y * 31 + 97) & 255))
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def run(binary, paths, output):
    data = subprocess.run(
        [binary.resolve(), *paths], check=True, text=True, capture_output=True
    ).stdout
    output.write_text(data, encoding="utf-8")
    rows = {}
    for line in data.splitlines():
        index, feature, *values = line.split()
        for component, value in enumerate(values):
            key = (int(index), feature, component)
            if key in rows:
                raise ValueError(f"{binary}: duplicate sample {key}")
            rows[key] = float(value)
    return rows


def compare(reference, actual):
    if reference.keys() != actual.keys() or not reference:
        raise ValueError("Sample indices, feature names, or component counts differ")
    differences = defaultdict(float)
    failures = []
    for key, expected in reference.items():
        _, label, _ = key
        observed = actual[key]
        error = abs(observed - expected)
        differences[label] = max(differences[label], error)
        # PBRT and rypath use binary32 except selected stable footprint and
        # Planck intermediates. Allow cancellation in filtered checker integrals
        # and slight accumulated noise and spectral reconstruction differences.
        tolerance = 3e-5 * max(1.0, abs(expected))
        if label in {"spectrum-blackbody", "spectrum-mix-1"}:
            # Pinned PBRT uses its approximate FastExp. This is the existing
            # absolute limit in docs/blackbody-reflectance-validation.md.
            tolerance = 2.5e-4
        if not math.isfinite(expected) or not math.isfinite(observed) or error > tolerance:
            failures.append(f"{key}: PBRT={expected}, rypath={observed}, tolerance={tolerance}")
    for label, maximum in sorted(differences.items()):
        print(f"{label:26} {maximum:.8g}")
    for failure in failures[:20]:
        print(failure)
    print(f"{len(reference)} comparisons across {len(differences)} features, {len(failures)} failures")
    return bool(failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pbrt_probe", type=Path)
    parser.add_argument("rypath_probe", type=Path)
    parser.add_argument("--output-directory", required=True, type=Path)
    args = parser.parse_args()
    directory = args.output_directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    rgb, gray, spectrum = (directory / filename for filename in ("rgb.png", "gray.png", "file.spd"))
    write_png(rgb, grayscale=False)
    write_png(gray, grayscale=True)
    spectrum.write_text("410 2\n565.5 4\n705 .4\n", encoding="utf-8")
    paths = rgb, gray, spectrum
    reference = run(args.pbrt_probe, paths, directory / "pbrt-graph.txt")
    actual = run(args.rypath_probe, paths, directory / "rypath-graph.txt")
    raise SystemExit(compare(reference, actual))


if __name__ == "__main__":
    main()
