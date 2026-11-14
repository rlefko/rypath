#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare checked rypath spectra with a separately linked pinned PBRT file oracle."""

import argparse
import hashlib
import math
import subprocess
from pathlib import Path

from extract_pbrt_rgb import REVISION


WAVELENGTHS = range(360, 831)
ENDPOINTS = {
    "cool.spd": (395, 640),
    "endpoint.spd": (360, 364),
    "sky.spd": (420, 680),
    "warm.spd": (475, 790),
}
SCENES = ("file-area", "file-background", "file-mixed", "file-ply-external")
PEAK_RELATIVE_TOLERANCE = 2e-6


def run_probe(executable, inputs):
    output = subprocess.check_output([executable.resolve(), *inputs], text=True, timeout=30)
    rows = [line.split() for line in output.splitlines()]
    if any(len(row) != len(WAVELENGTHS) + 2 for row in rows) or (
        [row[0] for row in rows] != list(ENDPOINTS)
    ):
        raise ValueError(f"{executable}: unexpected curve names or sample count")
    return output, {name: [float(value) for value in values] for name, *values in rows}


def compare(reference, actual):
    for name, endpoints in ENDPOINTS.items():
        peak, *expected = reference[name]
        actual_peak, *observed = actual[name]
        if not math.isfinite(peak) or peak <= 0 or actual_peak != peak:
            raise ValueError(f"{name}: peak differs from PBRT ({actual_peak} vs {peak})")
        limit = PEAK_RELATIVE_TOLERANCE * peak
        maximum_error = 0.0
        exact_checks = 0
        for wavelength, ref, test in zip(WAVELENGTHS, expected, observed, strict=True):
            exact = wavelength in endpoints or ref == 0
            error = abs(test - ref)
            tolerance = 0 if exact else limit
            if not (math.isfinite(ref) and ref >= 0 and test >= 0 and error <= tolerance):
                raise ValueError(
                    f"{name} at {wavelength} nm: PBRT {ref}, rypath {test}, tolerance {tolerance}"
                )
            maximum_error = max(maximum_error, error)
            exact_checks += exact
        print(
            f"{name}: max absolute error {maximum_error:.9g} (limit {limit:.9g}), "
            f"{exact_checks} exact endpoint or zero checks"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pbrt_source", type=Path)
    parser.add_argument("pbrt_probe", type=Path)
    parser.add_argument("rypath_probe", type=Path)
    parser.add_argument("fixtures", type=Path)
    parser.add_argument("--output-directory", type=Path)
    args = parser.parse_args()

    git = ["git", "-C", args.pbrt_source]
    revision = subprocess.check_output([*git, "rev-parse", "HEAD"], text=True).strip()
    modified = subprocess.check_output([*git, "status", "--porcelain"], text=True)
    if revision != REVISION or modified:
        raise ValueError(f"Expected a clean PBRT checkout at {REVISION}")

    assets = args.fixtures / "spectrum-assets"
    pbrt_output, reference = run_probe(args.pbrt_probe, [assets / name for name in ENDPOINTS])
    rypath_output, actual = run_probe(
        args.rypath_probe, [args.fixtures / f"{name}.pbrt" for name in SCENES]
    )
    compare(reference, actual)

    if args.output_directory:
        args.output_directory.mkdir(parents=True, exist_ok=True)
        for renderer, output in (("pbrt", pbrt_output), ("rypath", rypath_output)):
            path = args.output_directory / f"file-spectrum-{renderer}.txt"
            path.write_text(output, encoding="ascii")
            print(f"{path}: SHA-256 {hashlib.sha256(path.read_bytes()).hexdigest()}")
    print(
        f"PBRT {REVISION}: {len(ENDPOINTS)} spectra and "
        f"{len(ENDPOINTS) * len(WAVELENGTHS)} raw samples agree"
    )


if __name__ == "__main__":
    main()
