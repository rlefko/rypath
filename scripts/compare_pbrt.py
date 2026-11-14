#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare fixed linear RGB regions from rypath and a supplied pinned PBRT v4 build."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import struct
import subprocess
import sys
import tempfile
from typing import NamedTuple


WIDTH = HEIGHT = 8
REPLICATES = 16
FULL = (0, 0, WIDTH, HEIGHT)
TRILINEAR_BLEND = (4, 3, 5, 5)
POINT_NEAREST = (4, 3, 5, 5)
EWA_ELLIPSE = (4, 3, 5, 4)


class FilterControl(NamedTuple):
    region: str
    parameter: bytes
    original_value: bytes
    alternate_value: bytes
    alternate_name: str
    difference_name: str
    original_increases_red: bool


FILTER_CONTROLS = {
    "image-trilinear": (
        FilterControl("blend", b'"string filter"', b'"trilinear"', b'"bilinear"',
                      "bilinear", "bilinear-trilinear", False),
    ),
    "image-point": (
        FilterControl("nearest", b'"string filter"', b'"point"', b'"bilinear"',
                      "bilinear", "point-bilinear", True),
    ),
    "image-ewa": (
        FilterControl("ellipse", b'"string filter"', b'"ewa"', b'"trilinear"',
                      "trilinear", "ewa-trilinear", True),
        FilterControl("ellipse", b'"float maxanisotropy"', b'8', b'4',
                      "ewa4", "ewa8-ewa4", True),
    ),
}

# Scan the fixed fixture entries without treating comments, escaped quotes, or
# pieces of other values as parameter tokens.
PBRT_TOKEN = re.compile(rb'#[^\r\n]*|"(?:\\.|[^"\\])*"|[\[\]]|[^\s"#\[\]]+')

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
    "normals-diffuse": QUADRANTS,
    "normals-instances": QUADRANTS,
    "normals-emission": QUADRANTS,
    "cornell-box-attributes": QUADRANTS,
    "cornell-box-sobol-fastowen": QUADRANTS,
    "cornell-box-sobol-permutedigits": QUADRANTS,
    "cornell-box-sobol-owen": QUADRANTS,
    "ply-external": QUADRANTS,
    "ply-mixed": QUADRANTS,
    "ply-curved-emitter": QUADRANTS,
    "texture-diffuse": {"full": FULL},
    "texture-ply-external": QUADRANTS,
    "image-magnified": QUADRANTS,
    "image-minified": {
        "full": FULL,
        "intermediate-mip-left": (0, 0, 2, 8),
        "intermediate-mip-right": (2, 0, 4, 8),
        "coarsest-mip": (4, 0, 8, 8),
    },
    "image-ply": {
        **QUADRANTS,
        "curved-patch": (0, 1, 6, 7),
        "triangle": (7, 5, 8, 7),
    },
    "image-secondary": QUADRANTS,
    "image-trilinear": {"full": FULL, "blend": TRILINEAR_BLEND},
    "image-point": {"full": FULL, "nearest": POINT_NEAREST},
    "image-ewa": {"full": FULL, "ellipse": EWA_ELLIPSE},
    "blackbody-background": {"full": FULL},
    "blackbody-area": QUADRANTS,
    "blackbody-mixed": QUADRANTS,
    "blackbody-ply-external": QUADRANTS,
    "named-background": {"full": FULL},
    "named-area": QUADRANTS,
    "named-mixed": QUADRANTS,
    "named-ply-external": QUADRANTS,
    "file-background": {"full": FULL},
    "file-area": QUADRANTS,
    "file-mixed": QUADRANTS,
    "file-ply-external": QUADRANTS,
    "reflectance-named-diffuse": {"full": FULL},
    "reflectance-file-ply": QUADRANTS,
    "blackbody-reflectance-diffuse": {"full": FULL},
    "blackbody-reflectance-ply": QUADRANTS,
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


def stage_dependencies(fixture, destination):
    """Keep the declared dependency closure relative to each relocated entry."""
    manifest = fixture.parent / "dependencies.json"
    if not manifest.exists():
        return
    records = json.loads(manifest.read_text()).get(fixture.stem, [])
    for record in records:
        relative = Path(record["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"{manifest}: dependency must be entry-relative")
        payload = (fixture.parent / relative).read_bytes()
        if len(payload) != record["bytes"] or hashlib.sha256(payload).hexdigest() != record["sha256"]:
            raise ValueError(f"{relative}: dependency size or checksum mismatch")
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)


def render_pfm(backend, binary, variant, source):
    output = variant.with_suffix(".pfm")
    variant.write_bytes(source)
    command = [str(binary), str(variant), str(output)]
    if backend == "pbrt":
        command = [str(binary), "--quiet", "--outfile", str(output), str(variant)]
    subprocess.run(command, check=True, cwd=variant.parent, timeout=120)
    return read_pfm(output)


def render_replicates(rypath, pbrt, fixture, run_dir, regions):
    source = fixture.read_bytes()
    seed_token = b'"integer seed" [0]'
    if source.count(seed_token) != 1 or source.count(b'"integer pixelsamples" [4096]') != 1:
        raise ValueError(f"{fixture}: expected one sampler seed and 4096 spp")

    measurements = {}
    for backend, binary, start_seed in (("rypath", rypath, 0), ("pbrt", pbrt, REPLICATES)):
        backend_dir = run_dir / fixture.stem / backend
        backend_dir.mkdir(parents=True)
        stage_dependencies(fixture, backend_dir)
        measurements[backend] = {name: [] for name in regions}
        for seed in range(start_seed, start_seed + REPLICATES):
            variant = backend_dir / f"{seed:02}.pbrt"
            pixels = render_pfm(
                backend, binary, variant,
                source.replace(seed_token, f'"integer seed" [{seed}]'.encode(), 1),
            )
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


def make_filter_variant(scene, source, control):
    tokens = [token for token in PBRT_TOKEN.finditer(source) if not token[0].startswith(b"#")]
    parameter_words = control.parameter[1:-1].split()
    indices = [i for i, token in enumerate(tokens)
               if token[0].startswith(b'"') and token[0][1:-1].split() == parameter_words]
    expected = (f"{scene}: expected one {control.parameter.decode()} "
                f"with value {control.original_value.decode()}")
    if len(indices) != 1 or indices[0] + 1 == len(tokens):
        raise ValueError(expected)
    value_index = indices[0] + 1
    if tokens[value_index][0] == b"[":
        value_index += 1
        if value_index + 1 >= len(tokens) or tokens[value_index + 1][0] != b"]":
            raise ValueError(expected)
    value = tokens[value_index]
    if value[0] != control.original_value:
        raise ValueError(expected)
    return source[:value.start()] + control.alternate_value + source[value.end():]


def check_filter_sensitivity(scene, rypath, pbrt, run_dir, measurements):
    controls = FILTER_CONTROLS[scene]
    minimum = 0.02
    passed = True
    for backend, binary, seed in (("rypath", rypath, 0), ("pbrt", pbrt, REPLICATES)):
        backend_dir = run_dir / scene / backend
        source = (backend_dir / f"{seed:02}.pbrt").read_bytes()
        variants = [make_filter_variant(scene, source, control) for control in controls]
        for control, variant in zip(controls, variants):
            pixels = render_pfm(
                backend, binary, backend_dir / f"{seed:02}-{control.alternate_name}.pbrt", variant,
            )
            alternate_red = roi_means(pixels, REGIONS[scene][control.region])[0]
            original_red = measurements[backend][control.region][0][0]
            delta = original_red - alternate_red
            if not control.original_increases_red:
                delta = -delta
            ok = delta > minimum
            passed &= ok
            status = "PASS" if ok else "FAIL"
            print(f"{status} {scene} {control.region} R  {backend} {control.difference_name}={delta:.4g} "
                  f"minimum={minimum:.4g}")
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
            if scene in FILTER_CONTROLS:
                passed &= check_filter_sensitivity(scene, rypath, pbrt, run_dir, measurements)
        return 0 if passed else 1
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"compare_pbrt: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
