#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Extract the prepared knots and dense binary32 named spectra from pinned PBRT v4."""

import argparse
import math
import re
from bisect import bisect_right
from pathlib import Path

from extract_pbrt_rgb import FLOAT_PATTERN, REVISION, array, f32, lerp, read_pinned

LAMBDA_MIN = 360
LAMBDA_MAX = 830
WAVELENGTHS = range(LAMBDA_MIN, LAMBDA_MAX + 1)
SPECTRUM_COUNT = 88
NORMALIZED_COUNT = 16

FROM_INTERLEAVED = (
    r"PiecewiseLinearSpectrum::FromInterleaved\(\s*(\w+)\s*,\s*(true|false)\s*,\s*alloc\s*\)"
)


def parse_registry(source):
    """Resolve each initializer entry to its upstream array and normalization flag."""
    source = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)
    registry = re.search(r"\bnamedSpectra\s*=\s*\{(.*?)\};", source, re.S)
    if registry is None:
        raise ValueError("PBRT named spectrum registry is missing")

    aliases = {
        alias: (array_name, normalize)
        for alias, array_name, normalize in re.findall(
            rf"\bSpectrum\s+(\w+)\s*=\s*{FROM_INTERLEAVED}\s*;", source[: registry.start()]
        )
    }
    pattern = re.compile(rf'\{{\s*"([\w-]+)"\s*,\s*(?:{FROM_INTERLEAVED}|(\w+))\s*\}}')
    entries = {}
    remaining = registry[1].strip()
    while remaining:
        entry = pattern.match(remaining)
        if entry is None:
            raise ValueError(f"Unsupported PBRT registry entry: {remaining[:100]}")
        name, array_name, normalize, alias = entry.groups()
        if name in entries:
            raise ValueError(f"Duplicate PBRT spectrum name: {name}")
        if alias is not None:
            if alias not in aliases:
                raise ValueError(f"Unknown PBRT spectrum initializer: {alias}")
            array_name, normalize = aliases[alias]
        entries[name] = (array_name, normalize == "true")
        remaining = remaining[entry.end() :].lstrip()
        if remaining:
            if not remaining.startswith(","):
                raise ValueError(f"Invalid PBRT spectrum entry separator after {name}")
            remaining = remaining[1:].lstrip()
    return dict(sorted(entries.items()))


def interpolate(wavelength, knots, values):
    # PBRT uses the preceding interval for the last knot.
    index = min(bisect_right(knots, wavelength) - 1, len(knots) - 2)
    offset = f32(wavelength - knots[index])
    width = f32(knots[index + 1] - knots[index])
    return lerp(f32(offset / width), values[index], values[index + 1])


def prepare_knots(interleaved, cie_y, cie_y_integral):
    if len(interleaved) < 2 or len(interleaved) % 2:
        raise ValueError("Invalid interleaved spectrum length")
    knots = list(map(f32, interleaved[::2]))
    values = list(map(f32, interleaved[1::2]))
    if not all(map(math.isfinite, knots)) or any(a >= b for a, b in zip(knots, knots[1:])):
        raise ValueError("Spectrum wavelengths must be finite and strictly increasing")
    if any(not math.isfinite(value) or value < 0 for value in values):
        raise ValueError("Spectrum values must be finite and nonnegative")
    if knots[0] > LAMBDA_MIN:
        knots.insert(0, float(LAMBDA_MIN - 1))
        values.insert(0, values[0])
    if knots[-1] < LAMBDA_MAX:
        knots.append(float(LAMBDA_MAX + 1))
        values.append(values[-1])

    # PBRT extends and normalizes the knots before evaluating the dense spectrum.
    # Keep the padded intervals in the calculation so their rounding agrees too.
    if cie_y_integral is not None:
        photometric = 0.0
        for wavelength, y in zip(WAVELENGTHS, cie_y, strict=True):
            sample = interpolate(wavelength, knots, values)
            photometric = f32(photometric + f32(sample * y))
        if photometric <= 0:
            raise ValueError("Cannot normalize a zero-luminance PBRT spectrum")
        scale = f32(cie_y_integral / photometric)
        values = [f32(value * scale) for value in values]
    return knots, values


def densify(knots, values):
    return [interpolate(wavelength, knots, values) for wavelength in WAVELENGTHS]


def extract_named_knots(source, header):
    entries = parse_registry(source)
    normalized_count = sum(normalize for _, normalize in entries.values())
    if len(entries) != SPECTRUM_COUNT or normalized_count != NORMALIZED_COUNT:
        raise ValueError("Unexpected PBRT named spectrum registry dimensions")
    if array(source, "CIE_lambda") != list(WAVELENGTHS):
        raise ValueError("Unexpected PBRT CIE sampling grid")
    cie_y = list(map(f32, array(source, "CIE_Y")))
    integral = re.search(rf"\bCIE_Y_integral = ({FLOAT_PATTERN});", header)
    if integral is None:
        raise ValueError("PBRT CIE Y integral is missing")
    cie_y_integral = f32(float(integral[1]))
    return {
        name: prepare_knots(
            array(source, array_name), cie_y, cie_y_integral if normalize else None
        )
        for name, (array_name, normalize) in entries.items()
    }


def float_literal(value):
    mantissa, exponent = value.hex().split("p")
    return f"{mantissa.rstrip('0').rstrip('.')}p{exponent}f"


def attribution():
    return [
        "// SPDX-License-Identifier: Apache-2.0",
        "// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.",
        "// PhysLight code and measurements contributed by Anders Langlands and Luca Fascione",
        "// Copyright (c) 2020, Weta Digital, Ltd.",
        "// Glass data from https://refractiveindex.info under CC0-1.0.",
        f"// Generated from PBRT v4 {REVISION} by scripts/extract_pbrt_named_spectra.py.",
    ]


def make_include(spectra):
    lines = attribution() + [
        "// Samples at integer wavelengths 360–830 nm. See third_party/pbrt/README.md.",
        "constexpr auto names = std::to_array<std::string_view>({",
    ]
    lines.extend(f'    "{name}",' for name in spectra)
    lines.extend(["});", "", "constexpr std::array spectra{"])
    for name, values in spectra.items():
        lines.append(f"    DenseSpectrum{{ // {name}")
        literals = [float_literal(value) for value in values]
        lines.extend("        " + ", ".join(literals[i : i + 4]) + "," for i in range(0, len(values), 4))
        lines.append("    },")
    lines.append("};")
    return "\n".join(lines) + "\n"


def make_knots_include(spectra):
    lines = attribution() + [
        "// Prepared wavelength and value pairs. See third_party/pbrt/README.md.",
        "constexpr auto spectrum_knots = std::to_array<SpectralKnot>({",
    ]
    ranges = []
    offset = 0
    for name, (wavelengths, values) in spectra.items():
        lines.append(f"    // {name}")
        for wavelength, value in zip(wavelengths, values, strict=True):
            lines.append(f"    {{{float_literal(wavelength)}, {float_literal(value)}}},")
        ranges.append(f'    {{"{name}", {offset}, {len(wavelengths)}}},')
        offset += len(wavelengths)
    lines.extend(["});", "", "constexpr auto knot_ranges = std::to_array<KnotRange>({"])
    lines.extend(ranges)
    lines.append("});")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "pbrt_source", type=Path, help="PBRT Git checkout containing the pinned revision"
    )
    parser.add_argument("output", type=Path)
    parser.add_argument("--knots-output", type=Path, help="also write the prepared binary32 knots")
    args = parser.parse_args()

    prepared = extract_named_knots(
        read_pinned(args.pbrt_source, "spectrum.cpp"), read_pinned(args.pbrt_source, "spectrum.h")
    )
    spectra = {name: densify(*knots) for name, knots in prepared.items()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(make_include(spectra).encode("utf-8"))
    if args.knots_output is not None:
        args.knots_output.parent.mkdir(parents=True, exist_ok=True)
        args.knots_output.write_bytes(make_knots_include(prepared).encode("utf-8"))


if __name__ == "__main__":
    main()
