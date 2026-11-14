#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

from decimal import Decimal
import math
import sys
import unittest

from compare_blackbody_reflectance import (
    PBRT_WAVELENGTHS,
    WIEN_NM_K,
    binary64,
    check_analytic,
    checked_temperature,
    compare_analytic,
    compare_pbrt,
    parse_output,
    planck_values,
    query_wavelengths,
)


class BlackbodyReflectanceComparisonTests(unittest.TestCase):
    def test_queries_include_binary64_peak_neighbors_and_visible_endpoints(self):
        peak = float(WIEN_NM_K) / checked_temperature("4550.1234")
        queries = query_wavelengths(("4550.1234",))
        for wavelength in (360., math.nextafter(peak, -math.inf), peak,
                           math.nextafter(peak, math.inf), 830.):
            self.assertIn(wavelength, queries)

    def test_decimal_oracle_resolves_subnormals_and_temperature_extremes(self):
        wavelengths = (360.0, 830.0)
        values = planck_values(22.5, wavelengths)
        self.assertEqual(float(values[0]), 0.)
        self.assertTrue(0. < float(values[1]) < sys.float_info.min)
        cold = checked_temperature("1.401298464324817e-45")
        self.assertEqual(tuple(map(float, planck_values(cold, wavelengths))), (0., 0.))
        hot = checked_temperature("3.4028234663852886e38")
        blue, red = map(float, planck_values(hot, wavelengths))
        self.assertTrue(0. < red < blue < 1e-137)
        self.assertAlmostEqual(blue / red, (830 / 360)**4, places=12)

    def test_rejects_wrong_label_truncation_malformed_and_nonfinite_words(self):
        good = "3fe0000000000000"
        cases = ("other " + good, "3000", "3000 3fe000000000000x",
                 "3000 7ff8000000000000", "3000 7ff0000000000000",
                 "3000 8000000000000000", "3000 3ff0000000000001")
        for row in cases:
            with self.subTest(row=row), self.assertRaises(ValueError):
                parse_output(row, ("3000",), 1, binary64)
        self.assertEqual(parse_output("3000 " + good, ("3000",), 1, binary64),
                         {"3000": (0.5,)})

    def test_rejects_tiny_route_disagreement_before_tolerance(self):
        wavelength = 500.25
        expected = float(planck_values(3000., (wavelength,))[0])
        actual = {"3000/direct": (expected,),
                  "3000/texture": (math.nextafter(expected, math.inf),)}
        with self.assertRaisesRegex(ValueError, "direct and texture values disagree"):
            compare_analytic(("3000",), (wavelength,), actual)

    def test_rejects_wrong_normal_subnormal_zero_or_white(self):
        subnormal = math.ulp(0.)
        for expected, actual in ((Decimal("0.5"), 0.50000000001),
                                 (Decimal.from_float(subnormal), 0.),
                                 (Decimal.from_float(4 * subnormal), subnormal),
                                 (Decimal("1e-330"), subnormal),
                                 (Decimal(1), 1. - 3e-12)):
            with self.subTest(expected=expected, actual=actual):
                with self.assertRaisesRegex(ValueError, "analytic mismatch"):
                    check_analytic("control", 360.25, expected, actual)
        check_analytic("subnormal", 360.25, Decimal.from_float(4 * subnormal), 5 * subnormal)
        reference = 1e-309
        check_analytic("subnormal", 360.25, Decimal.from_float(reference),
                       reference + 100 * subnormal)
        check_analytic("peak", 637., Decimal(1), math.nextafter(1., 0.))

    def test_rejects_pbrt_difference_above_fixed_absolute_limit(self):
        reference = {"3000": (0.5,) * len(PBRT_WAVELENGTHS)}
        actual = {"3000/direct": (0.5003,) + reference["3000"][1:]}
        with self.assertRaisesRegex(ValueError, "3000 at 360.0 nm: PBRT mismatch"):
            compare_pbrt(("3000",), PBRT_WAVELENGTHS, reference, actual)


if __name__ == "__main__":
    unittest.main()
