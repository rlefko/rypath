#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

from fractions import Fraction
import unittest

from compare_pbrt_reflectance import WAVELENGTHS, compare as compare_pbrt, parse_output
from compare_reflectance_curves import (
    check_sample,
    compare,
    pack_curve,
    parse_curves,
    query_words,
    rational_curve,
    sample_exact,
    word,
)


class ReflectanceCurveComparisonTests(unittest.TestCase):
    def test_oracle_recovers_wide_interval_slope(self):
        knots = rational_curve(pack_curve((-2.0**120, -2.0**111), (2.0**120, 2.0**111)))
        wavelengths = [x for x, _ in knots]
        for wavelength, expected in ((360, Fraction(45, 64)), (512, Fraction(1)), (830, 1)):
            with self.subTest(wavelength=wavelength):
                self.assertEqual(sample_exact(knots, wavelengths, Fraction(wavelength)), expected)

    def test_queries_include_independent_crossing_neighbors(self):
        curve = {"step": pack_curve((511, -2.0**120), (513, 2.0**120))}
        coordinates = query_words(curve)["step"]
        for bits in range(word(512) - 1, word(512) + 2):
            self.assertIn(bits, coordinates)

    def test_rejects_curve_with_truncated_data(self):
        with self.assertRaisesRegex(ValueError, "unexpected knot or sample count"):
            parse_curves("control 2 1 43b40000 00000000 444f8000 3f800000 43b40000", ["control"])

    def test_rejects_nonfinite_or_malformed_word(self):
        for ordinate in ("7fc00000", "7f800000", "0000000x"):
            with self.subTest(ordinate=ordinate):
                row = f"control 2 1 43b40000 00000000 444f8000 3f800000 43b40000 {ordinate}"
                with self.assertRaisesRegex(ValueError, "binary32 word"):
                    parse_curves(row, ["control"])

    def test_rejects_preclamped_interpolation(self):
        source = {"ascending": pack_curve((400, -0.5), (600, 1.5))}
        premature_clamp = pack_curve((400, 0), (600, 1))
        wavelengths = [word(400) - 1, word(400), word(400) + 1, word(425),
                       word(600) - 1, word(600), word(600) + 1]
        sample_values = (0, 0, 0, 0.125, 1, 1, 0)
        compiled = {"ascending": (premature_clamp, list(zip(wavelengths, map(word, sample_values))))}
        with self.assertRaisesRegex(ValueError, "ascending at 425.0 nm"):
            compare(source, {"ascending": wavelengths}, compiled)

    def test_rejects_tiny_zero_and_one_perturbations(self):
        for expected, observed in ((0, 1e-8), (1, 1 - 1e-8)):
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(ValueError, "tolerance 0"):
                    check_sample("control", word(360), expected, observed, 1e-6)


class PbrtReflectanceComparisonTests(unittest.TestCase):
    def test_rejects_wrong_name_truncated_and_nonfinite_rows(self):
        valid = ["control", *(["3f000000"] * len(WAVELENGTHS))]
        variants = (["different", *valid[1:]], valid[:-1], [*valid[:-1], "7fc00000"])
        for row in variants:
            with self.subTest(last_word=row[-1], row_name=row[0], size=len(row)):
                with self.assertRaises(ValueError):
                    parse_output(" ".join(row), ["control"])

    def test_rejects_route_disagreement_below_tolerance(self):
        expected = [0.5] * len(WAVELENGTHS)
        observed = expected.copy()
        observed[0] += 1e-7
        with self.assertRaisesRegex(ValueError, "direct and texture values disagree"):
            compare_pbrt(["control"], {"control": expected},
                         {"control/direct": expected, "control/texture": observed})

    def test_rejects_out_of_tolerance_even_when_routes_agree(self):
        expected = [0.5] * len(WAVELENGTHS)
        observed = expected.copy()
        observed[0] += 3e-6
        with self.assertRaisesRegex(ValueError, "control at 360.0 nm"):
            compare_pbrt(["control"], {"control": expected},
                         {"control/direct": observed, "control/texture": observed})


if __name__ == "__main__":
    unittest.main()
