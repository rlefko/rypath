# SPDX-License-Identifier: GPL-3.0-only
"""Offline tests for parsing and sampling the pinned PBRT registry format."""

import struct
import unittest
from pathlib import Path

from compare_pbrt_named_spectra import upstream_names
from extract_pbrt_named_spectra import (
    WAVELENGTHS,
    densify,
    f32,
    float_literal,
    make_knots_include,
    parse_registry,
    prepare_knots,
)


SYNTHETIC_REGISTRY = """
    Spectrum indexed = PiecewiseLinearSpectrum::FromInterleaved(First, true, alloc);
    namedSpectra = {
        {"two", PiecewiseLinearSpectrum::FromInterleaved(Second, false, alloc)},
        // {"commented-out", indexed},
        {"one", indexed},
    };
"""


class NamedSpectrumExtractionTests(unittest.TestCase):
    def test_alias_and_inline_registry_entries(self):
        result = parse_registry(SYNTHETIC_REGISTRY)
        self.assertEqual(list(result.items()), [("one", ("First", True)), ("two", ("Second", False))])

    def test_rejects_duplicate_unknown_and_unsupported_entries(self):
        for text, error in (
            ('{"one", indexed},', "Duplicate PBRT spectrum name"),
            ('{"two", unknown},', "Unknown PBRT spectrum initializer"),
            ('{"two", UnrecognizedFactory()},', "Unsupported PBRT registry entry"),
        ):
            source = SYNTHETIC_REGISTRY.replace(
                '{"two", PiecewiseLinearSpectrum::FromInterleaved(Second, false, alloc)},', text
            )
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, error):
                parse_registry(source)

    def test_integer_wavelength_knots_are_preserved(self):
        # Treat the archived normalized D65 samples as knots to test exact wavelength hits.
        path = Path(__file__).resolve().parents[1] / "third_party/pbrt/d65.inc"
        samples = [
            float.fromhex(token.strip()[:-1])
            for line in path.read_text(encoding="utf-8").splitlines()
            if not line.startswith("//")
            for token in line.strip().rstrip(",").split(",")
        ]
        interleaved = [value for pair in zip(WAVELENGTHS, samples, strict=True) for value in pair]
        self.assertEqual(densify(*prepare_knots(interleaved, (), None)), samples)

    def test_irregular_knots_and_normalization_use_binary32_operations(self):
        interleaved = [370.125, 0.2, 820.25, 0.8]
        raw = prepare_knots(interleaved, (), None)
        normalized = prepare_knots(interleaved, [f32(0.85)] * len(WAVELENGTHS), f32(106.856895))
        expected_knots = [359.0, 370.125, 820.25, 831.0]
        self.assertEqual(raw, (expected_knots, [f32(0.2)] * 2 + [f32(0.8)] * 2))
        self.assertEqual(normalized[0], expected_knots)
        self.assertEqual(struct.pack(">4f", *normalized[1]).hex(), "3ddac198" * 2 + "3edac198" * 2)
        wavelengths = (360, 400, 555, 830)
        for prepared, expected in (
            (raw, "3e4cccce3e7593f83ee492ad3f4ccccd"),
            (normalized, "3ddac1983e0328133e7426323edac198"),
        ):
            samples = densify(*prepared)
            selected = [samples[wavelength - 360] for wavelength in wavelengths]
            self.assertEqual(struct.pack(">4f", *selected).hex(), expected)

    def test_prepared_knots_are_emitted_with_registry_ranges(self):
        first = prepare_knots([359.25, 0.125, 831.5, 1.25], (), None)
        second = prepare_knots([360.5, 0.125, 831.5, 1.25], (), None)
        generated = make_knots_include({"first": first, "second": second})
        self.assertIn('{"first", 0, 2}', generated)
        self.assertIn('{"second", 2, 3}', generated)
        self.assertIn("{0x1.674p+8f, 0x1p-3f}", generated)
        self.assertIn("{0x1.9fcp+9f, 0x1.4p+0f}", generated)

    def test_float_literal_preserves_zero_and_binary32_precision(self):
        self.assertEqual(float_literal(0.0), "0x0p+0f")
        self.assertEqual(float_literal(1.0), "0x1p+0f")
        self.assertEqual(float_literal(f32(0.2)), "0x1.99999ap-3f")

    def test_reference_names_validate_the_complete_unique_inventory(self):
        entries = ",\n".join(f'{{"upstream-{index:02d}", opaqueInitializer}}' for index in range(88))
        source = "namedSpectra = {" + entries + "};"
        self.assertEqual(upstream_names(source), [f"upstream-{index:02d}" for index in range(88)])
        with self.assertRaisesRegex(ValueError, "Unexpected pinned PBRT registry keys"):
            upstream_names(source.replace("upstream-87", "upstream-86"))


if __name__ == "__main__":
    unittest.main()
