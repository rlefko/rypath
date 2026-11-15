#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare all 47 homogeneous measured presets to pinned PBRT, without downloads.

The PBRT probe is scripts/pbrt_medium_probe.cpp linked to the existing pinned
binary32 reference. The rypath probe is rypath_medium_scene_tests --dump-presets.
RGB and 3000/6500 K blackbody coefficients/emission are also compared.
Evidence includes exact output and artifact digests; no external asset is copied.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import shlex
import subprocess
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PIN = "b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    return subprocess.run([str(value) for value in command], check=True, text=True,
                          capture_output=True, timeout=120, **kwargs).stdout


def parse(text, count=942):
    rows = {}
    for line in text.splitlines():
        fields = line.rsplit(" ", count)
        if len(fields) != count + 1:
            raise ValueError(f"Expected name and {count} spectral words")
        name, *words = fields
        if name in rows or any(re.fullmatch(r"[0-9a-f]{8}", word) is None for word in words):
            raise ValueError(f"Malformed or duplicate preset row: {name}")
        if any(not math.isfinite(value) or value < 0
               for value in (struct.unpack(">f", bytes.fromhex(word))[0] for word in words)):
            raise ValueError(f"Nonfinite or negative spectral value: {name}")
        rows[name] = words
    return rows


def coefficients(text, upstream):
    pattern = (r'\{"([^"]+)",\s*RGB\(([^)]+)\),\s*RGB\(([^)]+)\)\}' if upstream else
               r'\{"([^"]+)",\s*\{([^}]+)\},\s*\{([^}]+)\}\}')
    return {name: [struct.pack("f", float(value.strip().removesuffix("f")))
                   for value in (s + "," + a).split(",")]
            for name, s, a in re.findall(pattern, text)}


def build_probe(destination, reference_build):
    """Reuse pinned PBRT's configured include/link settings without rebuilding it."""
    ninja = (reference_build / "build.ninja").read_text()
    includes = next(line.split("=", 1)[1] for line in ninja.splitlines()
                    if line.strip().startswith("INCLUDES =") and "source/src -" in line)
    start = ninja.index("build pbrt: ")
    block = ninja[start:ninja.index("\n\n", start)]
    libraries = next(line.split("=", 1)[1] for line in block.splitlines()
                     if line.strip().startswith("LINK_LIBRARIES ="))
    destination = destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    obj = destination.with_suffix(".o")
    commands = [
        ["clang++", "-std=c++17", "-O2", "-ffp-contract=off", "-Wno-deprecated-declarations",
         *shlex.split(includes), "-c", str(ROOT / "scripts/pbrt_medium_probe.cpp"), "-o", str(obj)],
        ["clang++", str(obj), "-o", str(destination), *shlex.split(libraries)],
    ]
    for command in commands:
        subprocess.run(command, cwd=reference_build, check=True, timeout=120)
    return commands


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pbrt_probe", type=Path)
    parser.add_argument("rypath_probe", type=Path)
    parser.add_argument("--reference-source", type=Path,
                        default=ROOT / ".cache/pbrt-reference/source")
    parser.add_argument("--build-probe", action="store_true",
                        help="Build pbrt_probe using the existing sibling reference build")
    parser.add_argument("--output", type=Path,
                        default=ROOT / ".cache/medium-validation/presets")
    args = parser.parse_args()
    revision = run(["git", "-C", args.reference_source, "rev-parse", "HEAD"]).strip()
    if revision != PIN:
        raise ValueError("Pinned PBRT reference revision mismatch")
    commands = build_probe(args.pbrt_probe, args.reference_source.parent / "build") if args.build_probe else []
    phase_queries, phase_expected = [], []
    for g in (-0.8, -0.3, 0, 0.3, 0.8):
        for cosine in (-1, -0.9, -0.5, 0, 0.5, 0.9, 1):
            # Round inputs to the same binary32 domain passed to PBRT.
            gf, cf = (struct.unpack("f", struct.pack("f", value))[0] for value in (g, cosine))
            phase_queries.append(f"0 0 1 {math.sqrt(1 - cf * cf):.9g} 0 {cf:.9g} {gf:.9g}")
            denominator = 1 + gf * gf + 2 * gf * cf
            phase_expected.append((1 - gf * gf) / (4 * math.pi * denominator ** 1.5))
    phase_reference = run([args.pbrt_probe.resolve(), "--phase"], input="\n".join(phase_queries) + "\n")
    phase_values = [float(value) for value in phase_reference.split()]
    if len(phase_values) != len(phase_expected) or any(
            not math.isfinite(a) or a < 0 or abs(a - b) > 1e-7 + 3e-5 * abs(b)
            for a, b in zip(phase_values, phase_expected)):
        raise ValueError("Pinned HG evaluation disagrees with independent double equations")
    table = ROOT / "third_party/pbrt/medium_presets.inc"
    source_coefficients = coefficients((args.reference_source / "src/pbrt/media.cpp").read_text(), True)
    if coefficients(table.read_text(), False) != source_coefficients:
        raise ValueError("Measured RGB coefficients differ from pinned PBRT")
    names = re.findall(r'^\s*\{"([^"]+)"', table.read_text(), re.MULTILINE)
    if len(names) != 47 or len(set(names)) != 47:
        raise ValueError("Expected exactly 47 unique measured presets")
    reference = run([args.pbrt_probe.resolve(), "--presets"], input="\n".join(names) + "\n")
    with tempfile.TemporaryDirectory(prefix="rypath-medium-presets-") as directory:
        actual = run([args.rypath_probe.resolve(), "--dump-presets", directory])
        spectra_actual = run([args.rypath_probe.resolve(), "--dump-spectra", directory])
    spectra_reference = "rgb" + run([args.pbrt_probe.resolve(), "--spectra"],
                                    input=".2 .6 1.1 .8 .3 .05 2 .5 1 .7 1.3\n")
    for temperature in (3000, 6500):
        spectra_reference += f"blackbody{temperature}" + run(
            [args.pbrt_probe.resolve(), "--blackbody"], input=f"{temperature} .7 1.3\n")
    spectra_expected_rows, spectra_actual_rows = parse(spectra_reference, 1413), parse(spectra_actual, 1413)
    if set(spectra_expected_rows) != set(spectra_actual_rows):
        raise ValueError("RGB/blackbody spectral case inventory mismatch")
    spectra_max_absolute = spectra_max_scaled = 0
    for name, expected_words in spectra_expected_rows.items():
        for index, (actual_word, expected_word) in enumerate(zip(spectra_actual_rows[name], expected_words)):
            actual_value, expected_value = (struct.unpack(">f", bytes.fromhex(word))[0]
                                            for word in (actual_word, expected_word))
            delta = abs(actual_value - expected_value)
            scaled = delta / max(1, abs(expected_value))
            spectra_max_absolute = max(spectra_max_absolute, delta)
            spectra_max_scaled = max(spectra_max_scaled, scaled)
            # Existing blackbody comparisons allow 2.5e-4 for PBRT FastExp
            # versus stable Planck evaluation; RGB retains its 1e-5 gate.
            gate = 2.5e-4 if name.startswith("blackbody") else 1e-5
            if scaled > gate:
                raise ValueError(f"{name}: dense spectral word {index} exceeds inherited {gate} gate: "
                                 f"{actual_word} != {expected_word}")
    expected_rows, actual_rows = parse(reference), parse(actual)
    if set(expected_rows) != set(names) or set(actual_rows) != set(names):
        raise ValueError("Preset inventory mismatch")
    # Inherit the established 1e-5 RGB reconstruction floor, scaled for
    # unbounded spectra. Stable sigmoid evaluation is not bitwise PBRT float.
    tolerance = 1e-5
    max_absolute = max_scaled = 0
    exact = 0
    for name in names:
        for index, (actual_word, expected_word) in enumerate(zip(actual_rows[name], expected_rows[name])):
            actual_value, expected_value = (struct.unpack(">f", bytes.fromhex(word))[0]
                                            for word in (actual_word, expected_word))
            delta = abs(actual_value - expected_value)
            scaled = delta / max(1, abs(expected_value))
            max_absolute, max_scaled = max(max_absolute, delta), max(max_scaled, scaled)
            exact += actual_word == expected_word
            if scaled > tolerance:
                raise ValueError(f"{name}: {'sigma_a' if index < 471 else 'sigma_s'} "
                                 f"at {360 + index % 471} nm exceeds {tolerance}: "
                                 f"{actual_word} != {expected_word}")
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "pbrt.txt").write_text(reference)
    (args.output / "rypath.txt").write_text(actual)
    (args.output / "spectra-pbrt.txt").write_text(spectra_reference)
    (args.output / "spectra-rypath.txt").write_text(spectra_actual)
    (args.output / "phase-queries.txt").write_text("\n".join(phase_queries) + "\n")
    (args.output / "phase-pbrt.txt").write_text(phase_reference)
    evidence = {
        "reference_revision": revision,
        "reference_probe_build_commands": commands,
        "presets": len(names), "wavelengths": [360, 830],
        "compared_binary32_values": 47 * 942,
        "coefficient_comparison": "282 measured RGB coefficients bitwise exact",
        "spectral_tolerance": "abs(delta) <= 1e-5 * max(1, abs(reference))",
        "max_absolute_error": max_absolute, "max_scaled_error": max_scaled,
        "bitwise_exact_spectral_values": exact,
        "rgb_blackbody_cases": 3, "rgb_blackbody_dense_values": 4239,
        "blackbody_tolerance": "abs(delta) <= 2.5e-4 * max(1, abs(reference)); existing FastExp adaptation",
        "rgb_blackbody_max_absolute_error": spectra_max_absolute,
        "rgb_blackbody_max_scaled_error": spectra_max_scaled,
        "pinned_HG_double_equation_cases": len(phase_expected),
        "preset_table_sha256": sha256(table),
        "binaries": {str(path.resolve()): sha256(path) for path in
                     (args.pbrt_probe, args.rypath_probe)},
        "pbrt_output_sha256": sha256(args.output / "pbrt.txt"),
        "rypath_output_sha256": sha256(args.output / "rypath.txt"),
        "spectra_pbrt_output_sha256": sha256(args.output / "spectra-pbrt.txt"),
        "spectra_rypath_output_sha256": sha256(args.output / "spectra-rypath.txt"),
    }
    (args.output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"47 homogeneous presets: 282 exact RGB coefficients; {47 * 942} dense samples pass; "
          f"max absolute/scaled errors {max_absolute:.9g}/{max_scaled:.9g}")
    print(f"RGB/blackbody extinction/emission: 4239 dense samples pass; "
          f"max absolute/scaled errors {spectra_max_absolute:.9g}/{spectra_max_scaled:.9g}")


if __name__ == "__main__":
    main()
