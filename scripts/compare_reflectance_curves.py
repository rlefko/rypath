#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check compiled diffuse curves against exact rational interpolation of binary32 inputs."""

import argparse
from bisect import bisect_right
from fractions import Fraction
import hashlib
import math
from pathlib import Path
import random
import re
import struct
import subprocess


ABSOLUTE_TOLERANCE = 1e-6
BINARY32_WORD = re.compile(r"[0-9a-f]{8}")


def word(value):
    return struct.unpack(">I", struct.pack(">f", value))[0]


def binary32(bits):
    return struct.unpack(">f", struct.pack(">I", bits))[0]


FIRST_VISIBLE = word(360)
LAST_VISIBLE = word(830)


def parse_word(token):
    if BINARY32_WORD.fullmatch(token) is None:
        raise ValueError(f"Malformed binary32 word {token!r}")
    bits = int(token, 16)
    if not math.isfinite(binary32(bits)):
        raise ValueError(f"Nonfinite binary32 word {token}")
    return bits


def binary32_neighbors(bits):
    return range(max(bits - 1, FIRST_VISIBLE), min(bits + 1, LAST_VISIBLE) + 1)


def pack_curve(*knots):
    return tuple((word(wavelength), word(value)) for wavelength, value in knots)


def finite_random_word(rng):
    while True:
        bits = rng.getrandbits(32)
        if math.isfinite(binary32(bits)):
            return bits


def input_curves():
    largest = binary32(0x7F7FFFFF)
    tiny = binary32(1)
    step = binary32(word(512) + 1)
    curves = {
        "clamp-ascending": pack_curve((400.25, -0.5), (544.75, 1.5), (722.5, 0.25)),
        "clamp-descending": pack_curve((400, 1.25), (600, -0.75), (700, 0.5), (830, 1)),
        "narrow-support": pack_curve((512, 0.25), (step, 1.5)),
        "single-visible-lower": pack_curve((-largest, -1), (360, 0.75)),
        "single-visible-upper": pack_curve((830, 0.75), (largest, 2)),
        "no-visible-below": pack_curve((-largest, -1), (359.75, 2)),
        "no-visible-above": pack_curve((830.25, 2), (largest, -1)),
        "wide-linear": pack_curve((-2.0**120, -2.0**111), (2.0**120, 2.0**111)),
        "wide-linear-descending": pack_curve((-2.0**120, 2.0**111), (2.0**120, -2.0**111)),
        "saturated-adjacent": pack_curve((511, -largest), (513, largest)),
        "saturated-adjacent-reverse": pack_curve((511, largest), (513, -largest)),
        "full-span": pack_curve((-largest, -largest), (largest, largest)),
        "one-sided-wide": pack_curve((-largest, largest), (831, 0)),
        "one-sided-wide-reverse": pack_curve((359, 0), (largest, largest)),
        "subnormal": pack_curve((400, -tiny), (600, tiny), (830, 1)),
        "flat-white": pack_curve((-largest, largest), (largest, largest)),
        "flat-black": pack_curve((-largest, -largest), (largest, -largest)),
    }
    rng = random.Random(0xA16ED0)
    for index in range(256):
        # Mix arbitrary finite bit patterns and visible wavelengths so wide
        # spans do not crowd out representable interior segments.
        wavelengths = (
            [finite_random_word(rng) for _ in range(4)]
            + [rng.randrange(FIRST_VISIBLE, LAST_VISIBLE + 1) for _ in range(2)]
        )
        ordinates = (word(-1.5), word(-0.125), word(0), word(0.25), word(0.875), word(1), word(2))
        knots = [
            (bits, rng.choice(ordinates) if i % 2 == 0 else finite_random_word(rng))
            for i, bits in enumerate(sorted(set(wavelengths), key=binary32))
        ]
        curves[f"binary32-{index:03d}"] = tuple(knots)
    return curves


def rational_curve(knots):
    return tuple((Fraction(binary32(x)), Fraction(binary32(y))) for x, y in knots)


def sample_exact(knots, wavelengths, x):
    if x < wavelengths[0] or x > wavelengths[-1]:
        return Fraction(0)
    index = min(bisect_right(wavelengths, x), len(wavelengths) - 1)
    a, ya = knots[index - 1]
    b, yb = knots[index]
    return min(Fraction(1), max(Fraction(0), (ya * (b - x) + yb * (x - a)) / (b - a)))


def query_words(curves):
    rng = random.Random(0x345D2)
    result = {}
    for name, knots in curves.items():
        queries = {word(n + 0.5) for n in range(360, 830)}
        queries.update((FIRST_VISIBLE, LAST_VISIBLE))
        queries.update(rng.randrange(FIRST_VISIBLE, LAST_VISIBLE + 1) for _ in range(32))
        for x, _ in knots:
            queries.update(binary32_neighbors(x))
        exact = rational_curve(knots)
        for (a, ya), (b, yb) in zip(exact, exact[1:]):
            for threshold in (0, 1):
                if min(ya, yb) < threshold < max(ya, yb):
                    crossing = a + (threshold - ya) * (b - a) / (yb - ya)
                    if 360 <= crossing <= 830:
                        # Converting through binary64 may pick either rounded
                        # float at a midpoint. Its immediate neighbors cover both.
                        queries.update(binary32_neighbors(word(float(crossing))))
        result[name] = sorted(queries)
    return result


def probe_input(curves, queries):
    lines = []
    for name, knots in curves.items():
        words = [f"{bits:08x}" for knot in knots for bits in knot]
        words.extend(f"{bits:08x}" for bits in queries[name])
        lines.append(f"{name} {len(knots)} {len(queries[name])} " + " ".join(words))
    return "\n".join(lines) + "\n"


def parse_curves(output, names):
    rows = [line.split() for line in output.splitlines()]
    if any(len(row) < 3 for row in rows) or [row[0] for row in rows] != list(names):
        raise ValueError("Unexpected curve probe names or row headers")
    result = {}
    for name, knot_count, sample_count, *data in rows:
        knots = int(knot_count)
        samples = int(sample_count)
        if knots < 2 or samples < 1 or len(data) != 2 * (knots + samples):
            raise ValueError(f"{name}: unexpected knot or sample count")
        words = list(map(parse_word, data))
        pairs = list(zip(words[::2], words[1::2], strict=True))
        result[name] = (pairs[:knots], pairs[knots:])
    return result


def check_sample(name, bits, expected, observed, tolerance):
    error = abs(float(expected) - observed)
    bound = 0 if expected in (0, 1) else tolerance
    if not (0 <= observed <= 1 and error <= bound):
        raise ValueError(
            f"{name} at {binary32(bits)} nm ({bits:08x}): "
            f"expected {float(expected):.17g}, observed {observed:.17g}, tolerance {bound}"
        )
    return error


def compare(curves, queries, compiled):
    exact_count = 0
    sample_count = 0
    max_error = 0.0
    for name, source in curves.items():
        knots, samples = compiled[name]
        compiled_wavelengths = [x for x, _ in knots]
        positions = [binary32(x) for x in compiled_wavelengths]
        if (
            len(knots) > 5 * len(source)
            or any(a >= b for a, b in zip(positions, positions[1:]))
            or (compiled_wavelengths[0], compiled_wavelengths[-1]) != (source[0][0], source[-1][0])
        ):
            raise ValueError(f"{name}: invalid compiled curve support, ordering, or size")
        values = dict(knots)
        if any(
            x not in values or binary32(values[x]) != min(1, max(0, binary32(y)))
            for x, y in source
        ):
            raise ValueError(f"{name}: compiled curve changed an authored knot")
        if any(not 0 <= binary32(y) <= 1 for _, y in knots):
            raise ValueError(f"{name}: compiled ordinates must be within [0,1]")
        sample_wavelengths = set(queries[name])
        for x in compiled_wavelengths:
            if FIRST_VISIBLE <= x <= LAST_VISIBLE:
                sample_wavelengths.update(binary32_neighbors(x))
        if [x for x, _ in samples] != sorted(sample_wavelengths):
            raise ValueError(f"{name}: unexpected wavelengths or missing crossing neighbors")

        exact = rational_curve(source)
        wavelengths = [x for x, _ in exact]
        for x, y in samples:
            expected = sample_exact(exact, wavelengths, Fraction(binary32(x)))
            error = check_sample(name, x, expected, binary32(y), ABSOLUTE_TOLERANCE)
            max_error = max(max_error, error)
            exact_count += expected in (0, 1)
            sample_count += 1
    return sample_count, exact_count, max_error


def write_dump(path, contents):
    data = contents.encode("ascii")
    path.write_bytes(data)
    print(f"{path}: SHA-256 {hashlib.sha256(data).hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rypath_probe", type=Path)
    parser.add_argument("--output-directory", type=Path)
    args = parser.parse_args()

    curves = input_curves()
    queries = query_words(curves)
    request = probe_input(curves, queries)
    output = subprocess.check_output([args.rypath_probe.resolve()], text=True, input=request, timeout=60)
    if args.output_directory:
        args.output_directory.mkdir(parents=True, exist_ok=True)
        write_dump(args.output_directory / "curve-oracle-input.txt", request)
        write_dump(args.output_directory / "curve-oracle-rypath.txt", output)
    samples, exact, maximum = compare(curves, queries, parse_curves(output, curves))
    print(f"{len(curves)} curves, {samples} samples, {exact} exact zero/one checks, "
          f"max absolute error {maximum:.9g} (limit {ABSOLUTE_TOLERANCE:.9g})")


if __name__ == "__main__":
    main()
