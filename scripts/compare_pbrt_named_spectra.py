#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare all compiled rypath named samples to the separately linked PBRT probe."""

import argparse
import hashlib
import re
import subprocess
from pathlib import Path

from extract_pbrt_rgb import REVISION, read_pinned


def upstream_names(source):
    # Only enumerate keys. PBRT itself resolves and samples the spectra.
    registry = re.search(r"\bnamedSpectra\s*=\s*\{(.*?)\};", source, re.S)
    if registry is None:
        raise ValueError("Pinned PBRT named spectrum registry is missing")
    names = sorted(re.findall(r'\{\s*"([^"]+)"\s*,', registry[1]))
    if len(names) != 88 or len(set(names)) != len(names):
        raise ValueError("Unexpected pinned PBRT registry keys")
    return names


def run_probe(command, names=None):
    output = subprocess.check_output(command, text=True, input=names)
    return [line.split() for line in output.splitlines()]


def compare(names, reference, actual):
    for label, rows in (("PBRT", reference), ("rypath", actual)):
        if any(len(row) != 472 for row in rows) or [row[0] for row in rows] != names:
            raise ValueError(f"{label} output disagrees with the pinned registry or sample count")
    for ref, test in zip(reference, actual, strict=True):
        for wavelength, (expected, observed) in enumerate(zip(ref[1:], test[1:], strict=True), 360):
            if expected != observed:
                raise ValueError(f"{ref[0]} at {wavelength} nm: PBRT {expected}, rypath {observed}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pbrt_source", type=Path)
    parser.add_argument("pbrt_probe", type=Path)
    parser.add_argument("rypath_test_executable", type=Path)
    parser.add_argument("--output-directory", type=Path)
    args = parser.parse_args()

    names = upstream_names(read_pinned(args.pbrt_source, "spectrum.cpp"))
    reference = run_probe([args.pbrt_probe.resolve()], "\n".join(names) + "\n")
    actual = run_probe([args.rypath_test_executable.resolve(), "--dump"])
    compare(names, reference, actual)

    if args.output_directory:
        args.output_directory.mkdir(parents=True, exist_ok=True)
        for label, rows in (("pbrt", reference), ("rypath", actual)):
            path = args.output_directory / f"named-spectrum-{label}.txt"
            path.write_text("".join(" ".join(row) + "\n" for row in rows), encoding="ascii")
            print(f"{path}: SHA-256 {hashlib.sha256(path.read_bytes()).hexdigest()}")
    print(f"PBRT {REVISION}: {len(names)} names and {len(names) * 471} binary32 samples match exactly")


if __name__ == "__main__":
    main()
