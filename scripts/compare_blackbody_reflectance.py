#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check translated blackbody diffuse reflectance against Planck and pinned PBRT."""

import argparse
from decimal import Decimal, localcontext
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory

from compare_reflectance_curves import binary32, parse_word, word, write_dump
from extract_pbrt_rgb import REVISION


PBRT_TEMPERATURES = ("1000", "2850", "3000", "6500", "15000", "50000")
ANALYTIC_TEMPERATURES = (
    *PBRT_TEMPERATURES,
    "1.401298464324817e-45",
    "1.175494350822288e-38",
    "22.5",
    "23",
    "30",
    "100",
    "3491.2917",
    "4550.1234",
    "8049.3669",
    "1e20",
    "3.4028234663852886e38",
)
PBRT_WAVELENGTHS = tuple(n / 4 for n in range(4 * 360, 4 * 830 + 1))
PBRT_TOLERANCE = 2.5e-4
RELATIVE_TOLERANCE = Decimal("2e-12")
WIEN_NM_K = Decimal("2897772.1")
ROUTES = ("direct", "texture")


def checked_temperature(authored):
    return binary32(word(float(authored)))


def query_wavelengths(temperatures):
    wavelengths = set(PBRT_WAVELENGTHS)
    wavelengths.update((360.0001, 400.251, 503.123456789, 719.99999, 829.9999))
    for text in temperatures:
        peak = float(WIEN_NM_K) / checked_temperature(text)
        if 360 <= peak <= 830:
            wavelengths.update((math.nextafter(peak, -math.inf), peak,
                                math.nextafter(peak, math.inf)))
            peak_word = word(peak)
            wavelengths.update(binary32(bits) for bits in range(peak_word - 1, peak_word + 2))
    return tuple(sorted(wavelength for wavelength in wavelengths if 360 <= wavelength <= 830))


def scene_source(temperatures):
    lines = [
        'Camera "perspective"',
        'Film "rgb"',
        'PixelFilter "box"',
        'Sampler "independent"',
        'Integrator "path"',
        'WorldBegin',
    ]
    shape = 'Shape "trianglemesh" "point3 P" [0 0 2 1 0 2 0 1 2] "integer indices" [0 1 2]'
    for index, temperature in enumerate(temperatures):
        lines.extend((
            f'Material "diffuse" "blackbody reflectance" [{temperature}]',
            shape,
            f'Texture "paint{index}" "spectrum" "constant" "blackbody value" [{temperature}]',
            f'Material "diffuse" "texture reflectance" "paint{index}"',
            shape,
        ))
    return "\n".join(lines) + "\n"


def binary64(token):
    if re.fullmatch(r"[0-9a-f]{16}", token) is None:
        raise ValueError(f"Invalid binary64 word {token!r}")
    return struct.unpack(">d", bytes.fromhex(token))[0]


def parse_output(output, labels, sample_count, decode):
    rows = [line.split() for line in output.splitlines()]
    if any(len(row) != 1 + sample_count for row in rows) or (
        [row[0] for row in rows] != list(labels)
    ):
        raise ValueError("Unexpected probe labels or sample count")
    values = {name: tuple(decode(token) for token in samples) for name, *samples in rows}
    if any(not 0 <= value <= 1 or math.copysign(1, value) < 0
           for samples in values.values() for value in samples):
        raise ValueError("Probe reflectance must be finite and in [0, 1] with positive zero")
    return values


def planck_values(temperature, wavelengths):
    """Evaluate absolute Planck radiance in SI at each wavelength and at the peak."""
    with localcontext() as context:
        context.prec = 80
        h = Decimal("6.62606957e-34")
        c = Decimal("299792458")
        k = Decimal("1.3806488e-23")
        kelvin = Decimal.from_float(temperature)
        peak = WIEN_NM_K / kelvin

        def radiance(wavelength):
            meters = wavelength * Decimal("1e-9")
            exponent = h * c / (meters * k * kelvin)
            # The normalized form is proportional to x^5/(exp(x)-1).
            # It decreases for x >= 1000, where it is already below half
            # the smallest binary64 subnormal. Avoid enormous Decimal exp.
            if exponent >= 1000:
                return Decimal(0)
            return 2 * h * c**2 / (meters**5 * (exponent.exp() - 1))

        peak_radiance = radiance(peak)
        return tuple(min(Decimal(1), radiance(Decimal.from_float(wavelength)) / peak_radiance)
                     for wavelength in wavelengths)


def check_analytic(label, wavelength, expected, actual):
    rounded = float(expected)
    if rounded == 0:
        valid = actual == 0
        error = 0
    elif rounded < sys.float_info.min:
        # Allow the same relative error before rounding, plus one subnormal step.
        absolute_error = abs(Decimal.from_float(actual) - expected)
        tolerance = RELATIVE_TOLERANCE * expected + Decimal.from_float(math.ulp(0.0))
        valid = actual > 0 and absolute_error <= tolerance
        error = 0
    else:
        error = abs(Decimal.from_float(actual) - expected) / expected
        valid = error <= RELATIVE_TOLERANCE
    if not valid:
        raise ValueError(f"{label} at {wavelength!r} nm: analytic mismatch, "
                         f"actual {actual:.17g}, expected {expected:.17g}")
    return error


def compare_analytic(temperatures, wavelengths, actual):
    maximum = 0
    zero_count = subnormal_count = white_count = 0
    for label in temperatures:
        expected = planck_values(checked_temperature(label), wavelengths)
        for wavelength, reference, direct, texture in zip(
            wavelengths, expected, actual[f"{label}/direct"], actual[f"{label}/texture"], strict=True
        ):
            if direct != texture:
                raise ValueError(f"{label} at {wavelength!r} nm: direct and texture values disagree")
            maximum = max(maximum, check_analytic(label, wavelength, reference, direct))
            rounded = float(reference)
            zero_count += rounded == 0
            subnormal_count += 0 < rounded < sys.float_info.min
            white_count += rounded == 1
    return maximum, zero_count, subnormal_count, white_count


def compare_pbrt(temperatures, wavelengths, reference, actual):
    indices = {wavelength: index for index, wavelength in enumerate(wavelengths)}
    maximum = 0.0
    for label in temperatures:
        direct = actual[f"{label}/direct"]
        for wavelength, expected in zip(PBRT_WAVELENGTHS, reference[label], strict=True):
            error = abs(direct[indices[wavelength]] - expected)
            if error > PBRT_TOLERANCE:
                raise ValueError(f"{label} at {wavelength!r} nm: PBRT mismatch, "
                                 f"absolute error {error:.9g} exceeds {PBRT_TOLERANCE:.9g}")
            maximum = max(maximum, error)
    return maximum


def run(args, directory):
    entry = directory / "blackbody-reflectance-probe.pbrt"
    entry.write_text(scene_source(ANALYTIC_TEMPERATURES), encoding="ascii")
    labels = [f"{name}/{route}" for name in ANALYTIC_TEMPERATURES for route in ROUTES]
    wavelengths = query_wavelengths(ANALYTIC_TEMPERATURES)
    query = "\n".join((str(len(wavelengths)), *map(repr, wavelengths), *labels)) + "\n"
    actual_output = subprocess.check_output(
        [args.rypath_probe.resolve(), "--scene-double", entry],
        text=True, input=query, timeout=60,
    )
    pbrt_output = subprocess.check_output(
        [args.pbrt_probe.resolve()], text=True,
        input="".join(f"{temperature} {temperature}\n" for temperature in PBRT_TEMPERATURES),
        timeout=60,
    )
    if args.output_directory:
        write_dump(directory / "blackbody-reflectance-queries.txt", query)
        write_dump(directory / "blackbody-reflectance-rypath.txt", actual_output)
        write_dump(directory / "blackbody-reflectance-pbrt.txt", pbrt_output)
    actual = parse_output(actual_output, labels, len(wavelengths), binary64)
    reference = parse_output(pbrt_output, PBRT_TEMPERATURES, len(PBRT_WAVELENGTHS),
                             lambda token: binary32(parse_word(token)))
    maximum, zeros, subnormals, whites = compare_analytic(ANALYTIC_TEMPERATURES, wavelengths, actual)
    print(f"Decimal Planck: {len(ANALYTIC_TEMPERATURES)} temperatures, "
          f"{len(wavelengths) * len(ANALYTIC_TEMPERATURES)} samples per route, "
          f"{zeros} exact zeros, {subnormals} subnormals, {whites} reference whites, "
          f"maximum normal relative error {maximum:.9g} (limit {RELATIVE_TOLERANCE:.9g})")
    pbrt_error = compare_pbrt(PBRT_TEMPERATURES, wavelengths, reference, actual)
    print(f"PBRT {REVISION}: {len(PBRT_TEMPERATURES)} temperatures, "
          f"{len(PBRT_WAVELENGTHS) * len(PBRT_TEMPERATURES)} samples per route, "
          f"maximum absolute error {pbrt_error:.9g} (limit {PBRT_TOLERANCE:.9g})")


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
    if args.output_directory:
        args.output_directory.mkdir(parents=True, exist_ok=True)
        run(args, args.output_directory)
    else:
        with TemporaryDirectory() as directory:
            run(args, Path(directory))


if __name__ == "__main__":
    main()
