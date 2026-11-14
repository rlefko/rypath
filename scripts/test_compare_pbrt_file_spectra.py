#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

from pathlib import Path
import unittest
from unittest import mock

from compare_pbrt_file_spectra import ENDPOINTS, WAVELENGTHS, compare, run_probe


class FileSpectrumComparisonTests(unittest.TestCase):
    def setUp(self):
        self.reference = {
            name: [1.0] + [float(low <= nm <= high) for nm in WAVELENGTHS]
            for name, (low, high) in ENDPOINTS.items()
        }
        self.actual = {name: values.copy() for name, values in self.reference.items()}

    def test_rejects_truncated_probe_row(self):
        rows = [f"{name} " + " ".join(str(value) for value in values)
                for name, values in self.reference.items()]
        rows[-1] = rows[-1].rsplit(" ", 1)[0]
        with mock.patch("subprocess.check_output", return_value="\n".join(rows)):
            with self.assertRaisesRegex(ValueError, "unexpected curve names or sample count"):
                run_probe(Path("probe"), [])

    def test_rejects_nonfinite_or_out_of_tolerance_interior(self):
        for value in (float("inf"), float("nan"), 1.000003):
            with self.subTest(value=value):
                self.actual["cool.spd"][400 - 360 + 1] = value
                with self.assertRaisesRegex(ValueError, "cool.spd at 400 nm"):
                    compare(self.reference, self.actual)

    def test_rejects_endpoint_or_zero_perturbations_below_tolerance(self):
        for nm in (394, 395, 640, 641):
            with self.subTest(wavelength=nm):
                index = nm - 360 + 1
                self.actual["cool.spd"][index] += 1e-7
                with self.assertRaisesRegex(ValueError, f"cool.spd at {nm} nm"):
                    compare(self.reference, self.actual)
                self.actual["cool.spd"][index] = self.reference["cool.spd"][index]


if __name__ == "__main__":
    unittest.main()
