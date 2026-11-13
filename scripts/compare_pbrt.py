#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare fixed linear RGB regions from rypath and a supplied pinned PBRT v4 build."""

import argparse
import math
from pathlib import Path
import statistics
import struct
import subprocess
import sys
import tempfile


WIDTH = HEIGHT = 8
REPLICATES = 16
FULL = (0, 0, WIDTH, HEIGHT)
QUADRANTS = {
    "full": FULL,
    "top-left": (0, 0, 4, 4),
    "top-right": (4, 0, 8, 4),
    "bottom-left": (0, 4, 4, 8),
    "bottom-right": (4, 4, 8, 8),
}
REGIONS = {
    "background": {"full": FULL},
    "diffuse": {"full": FULL},
    "rgb-background": {"full": FULL},
    "rgb-diffuse": {"full": FULL},
    "rgb-multibounce": QUADRANTS,
    "multibounce": QUADRANTS,
    "area-visible": QUADRANTS,
    "area-direct": QUADRANTS,
    "area-mixed": QUADRANTS,
    "cornell-box-adapted": QUADRANTS,
    "triangle-filter": QUADRANTS,
}


def read_pfm(path, width=WIDTH, height=HEIGHT):
    """Read RGB PFM with unit scale and return top-down row-major linear RGB."""
    with path.open("rb") as file:
        magic, dimensions, scale = (file.readline(64) for _ in range(3))
        if magic != b"PF\n":
            raise ValueError(f"{path}: expected an RGB PFM")
        if any(not line.endswith(b"\n") for line in (dimensions, scale)):
            raise ValueError(f"{path}: missing or oversized PFM header field")
        if dimensions.split() != [str(width).encode(), str(height).encode()]:
            raise ValueError(f"{path}: expected PFM dimensions {width}x{height}")
        byte_order = {1.0: ">", -1.0: "<"}.get(float(scale))
        if byte_order is None:
            raise ValueError(f"{path}: expected a PFM with unit scale")
        byte_count = width * height * 3 * 4
        payload = file.read(byte_count + 1)
        if len(payload) != byte_count:
            raise ValueError(f"{path}: incorrect PFM payload byte count")
    bottom_up = list(struct.iter_unpack(byte_order + "fff", payload))
    if any(not math.isfinite(value) for pixel in bottom_up for value in pixel):
        raise ValueError(f"{path}: nonfinite PFM channel")
    return [
        pixel
        for y in reversed(range(height))
        for pixel in bottom_up[y * width : (y + 1) * width]
    ]


def roi_means(pixels, bounds):
    x0, y0, x1, y1 = bounds
    roi = [pixels[y * WIDTH + x] for y in range(y0, y1) for x in range(x0, x1)]
    return tuple(statistics.mean(values) for values in zip(*roi))


def render_replicates(rypath, pbrt, fixture, run_dir, regions):
    source = fixture.read_bytes()
    seed_token = b'"integer seed" [0]'
    if source.count(seed_token) != 1 or source.count(b'"integer pixelsamples" [4096]') != 1:
        raise ValueError(f"{fixture}: expected one independent-sampler seed and 4096 spp")

    measurements = {}
    for backend, binary, start_seed in (("rypath", rypath, 0), ("pbrt", pbrt, REPLICATES)):
        backend_dir = run_dir / fixture.stem / backend
        backend_dir.mkdir(parents=True)
        measurements[backend] = {name: [] for name in regions}
        for seed in range(start_seed, start_seed + REPLICATES):
            variant = backend_dir / f"{seed:02}.pbrt"
            output = variant.with_suffix(".pfm")
            variant.write_bytes(source.replace(seed_token, f'"integer seed" [{seed}]'.encode(), 1))
            command = [str(binary), str(variant), str(output)]
            if backend == "pbrt":
                command = [str(binary), "--quiet", "--outfile", str(output), str(variant)]
            subprocess.run(command, check=True, cwd=backend_dir, timeout=120)
            pixels = read_pfm(output)
            for name, bounds in regions.items():
                measurements[backend][name].append(roi_means(pixels, bounds))
    return measurements


def compare_regions(scene, measurements):
    passed = True
    for region, replicates in measurements["rypath"].items():
        pbrt_replicates = measurements["pbrt"][region]
        for channel, ours, reference in zip("RGB", zip(*replicates), zip(*pbrt_replicates)):
            ours_mean = statistics.mean(ours)
            ref_mean = statistics.mean(reference)
            combined_se = math.sqrt(
                statistics.variance(ours) / len(ours) + statistics.variance(reference) / len(reference)
            )
            threshold = 5 * combined_se + 2e-5 * max(1, abs(ref_mean))
            difference = abs(ours_mean - ref_mean)
            ok = difference <= threshold
            passed &= ok
            status = "PASS" if ok else "FAIL"
            print(
                f"{status} {scene} {region} {channel}  rypath={ours_mean:.8g} "
                f"pbrt={ref_mean:.8g} delta={difference:.4g} "
                f"combined_se={combined_se:.4g} limit={threshold:.4g}"
            )
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rypath", type=Path, required=True, help="rypath_pbrt_reference binary")
    parser.add_argument("--pbrt", type=Path, required=True, help="pinned upstream pbrt binary")
    parser.add_argument("--fixtures", type=Path, required=True, help="PBRT render fixture directory")
    parser.add_argument("--output-dir", type=Path, required=True, help="cache for variants and PFM outputs")
    args = parser.parse_args()
    try:
        rypath = args.rypath.resolve(strict=True)
        pbrt = args.pbrt.resolve(strict=True)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        run_dir = Path(tempfile.mkdtemp(prefix="pbrt-compare-", dir=args.output_dir.resolve()))
        print(f"Comparison artifacts in {run_dir}", flush=True)
        passed = True
        for scene, regions in REGIONS.items():
            fixture = args.fixtures / f"{scene}.pbrt"
            measurements = render_replicates(rypath, pbrt, fixture, run_dir, regions)
            passed &= compare_regions(scene, measurements)
        return 0 if passed else 1
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"compare_pbrt: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
