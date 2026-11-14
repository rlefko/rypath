#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare translated diffuse reflectance with a separately linked pinned PBRT probe."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from compare_pbrt_named_spectra import upstream_names
from compare_reflectance_curves import binary32, check_sample, parse_word, word, write_dump
from extract_pbrt_rgb import REVISION, read_pinned


WAVELENGTHS = tuple(word(n / 4) for n in range(4 * 360, 4 * 830 + 1))
ABSOLUTE_TOLERANCE = 2e-6


def scene_source(names):
    lines = [
        'Camera "perspective"',
        'Film "rgb"',
        'PixelFilter "box"',
        'Sampler "independent"',
        'Integrator "path"',
        'WorldBegin',
    ]
    for index, name in enumerate(names):
        # Exercise the direct and constant-texture paths against the same
        # reference curve, with two explicit geometry bindings per name.
        lines += [
            f'Material "diffuse" "spectrum reflectance" "{name}"',
            'Shape "trianglemesh" "point3 P" [0 0 2 1 0 2 0 1 2] "integer indices" [0 1 2]',
            f'Texture "paint{index}" "spectrum" "constant" "spectrum value" "{name}"',
            f'Material "diffuse" "texture reflectance" "paint{index}"',
            'Shape "trianglemesh" "point3 P" [0 0 2 1 0 2 0 1 2] "integer indices" [0 1 2]',
        ]
    return "\n".join(lines) + "\n"


def parse_output(output, names):
    rows = [line.split() for line in output.splitlines()]
    if any(len(row) != 1 + len(WAVELENGTHS) for row in rows) or (
        [row[0] for row in rows] != list(names)
    ):
        raise ValueError("Unexpected probe names or sample count")
    return {name: [binary32(parse_word(token)) for token in samples] for name, *samples in rows}


def compare(names, reference, actual):
    maximum_error = 0.0
    zero_checks = 0
    one_checks = 0
    for name in names:
        expected = reference[name]
        direct = actual[f"{name}/direct"]
        texture = actual[f"{name}/texture"]
        for wavelength, ref, direct_value, texture_value in zip(
            WAVELENGTHS, expected, direct, texture, strict=True
        ):
            if not 0 <= ref <= 1:
                raise ValueError(f"{name} at {binary32(wavelength)} nm: PBRT value out of range")
            if direct_value != texture_value:
                raise ValueError(
                    f"{name} at {binary32(wavelength)} nm: direct and texture values disagree"
                )
            error = check_sample(name, wavelength, ref, direct_value, ABSOLUTE_TOLERANCE)
            maximum_error = max(maximum_error, error)
            zero_checks += ref == 0
            one_checks += ref == 1
    return maximum_error, zero_checks, one_checks


def run(args, directory, names):
    entry = directory / "named-reflectance-probe.pbrt"
    scene = scene_source(names)
    if args.output_directory:
        write_dump(entry, scene)
    else:
        entry.write_text(scene, encoding="ascii")
    labels = [f"{name}/{route}" for name in names for route in ("direct", "texture")]
    reference = subprocess.check_output(
        [args.pbrt_probe.resolve()], text=True, input="\n".join(names) + "\n", timeout=60
    )
    actual = subprocess.check_output(
        [args.rypath_probe.resolve(), "--scene", entry],
        text=True,
        input="\n".join(labels) + "\n",
        timeout=60,
    )
    if args.output_directory:
        write_dump(directory / "named-reflectance-pbrt.txt", reference)
        write_dump(directory / "named-reflectance-rypath.txt", actual)
    return compare(names, parse_output(reference, names), parse_output(actual, labels))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pbrt_source", type=Path)
    parser.add_argument("pbrt_probe", type=Path)
    parser.add_argument("rypath_probe", type=Path)
    parser.add_argument("--output-directory", type=Path)
    args = parser.parse_args()

    git = ["git", "-C", args.pbrt_source]
    revision = subprocess.check_output([*git, "rev-parse", "HEAD"], text=True).strip()
    modified = subprocess.check_output([*git, "status", "--porcelain"], text=True)
    if revision != REVISION or modified:
        raise ValueError(f"Expected a clean PBRT checkout at {REVISION}")
    names = upstream_names(read_pinned(args.pbrt_source, "spectrum.cpp"))
    if args.output_directory:
        args.output_directory.mkdir(parents=True, exist_ok=True)
        maximum, zeros, ones = run(args, args.output_directory, names)
    else:
        with TemporaryDirectory() as directory:
            maximum, zeros, ones = run(args, Path(directory), names)
    print(f"PBRT {REVISION}: {len(names)} spectra, {len(names) * len(WAVELENGTHS)} samples "
          f"per route, {zeros} exact zero and {ones} exact white checks per route, "
          f"max absolute error {maximum:.9g} (limit {ABSOLUTE_TOLERANCE:.9g})")


if __name__ == "__main__":
    main()
