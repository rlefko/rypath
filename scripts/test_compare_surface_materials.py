#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check immutable matched sources and statistical gates without a renderer."""
import tempfile
from pathlib import Path
import unittest

from compare_surface_materials import SCENES, FIXTURES, staged_source, statistics_record


class SurfaceComparisonTests(unittest.TestCase):
    def test_owned_sources_match_linear_settings_and_manifest(self):
        for name in SCENES:
            if name == 'measured':
                continue
            source = staged_source(name, None)
            self.assertEqual(source, (FIXTURES/(name+'.pbrt')).read_bytes())
            for token in (b'"integer pixelsamples" [4096]', b'"integer seed" [0]', b'"string sensor" "cie1931"', b'PixelFilter "box"'):
                self.assertIn(token, source)
        self.assertNotIn('fourier', SCENES)

    def test_measured_substitution_is_explicit_and_does_not_modify_fixture(self):
        original = (FIXTURES/'measured.pbrt').read_bytes()
        with tempfile.TemporaryDirectory() as directory:
            table = Path(directory)/'fixture.tensor'
            table.write_bytes(b'tensor_file\0fixture')
            source = staged_source('measured', table)
            self.assertEqual(source, original.replace(b'$MEASURED_TABLE', str(table.resolve()).encode()))
            self.assertEqual((FIXTURES/'measured.pbrt').read_bytes(), original)
            table.write_bytes(b'SCATFUN\x01')
            with self.assertRaisesRegex(ValueError, 'tensor signature'):
                staged_source('measured', table)
        with self.assertRaises(ValueError):
            staged_source('measured', None)

    def test_fixed_gate_uses_both_independent_variances_and_detects_bias(self):
        values = [(1,2,3), (1.1,2.1,3.1), (.9,1.9,2.9), (1,2,3)]*2
        accepted = statistics_record({'rypath': {'full': values}, 'pbrt': {'full': values}})
        self.assertTrue(all(record['passed'] and record['combined_se'] > 0 for record in accepted))
        biased = [(a+1,b+1,c+1) for a,b,c in values]
        rejected = statistics_record({'rypath': {'full': biased}, 'pbrt': {'full': values}})
        self.assertTrue(all(not record['passed'] for record in rejected))


if __name__ == '__main__':
    unittest.main()
