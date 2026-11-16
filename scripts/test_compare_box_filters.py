#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Box comparison protocol and complete fixture closure without rendering."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import compare_box_filters as suite


class BoxComparisonTests(unittest.TestCase):
    def test_variants_and_dependency_closure(self):
        self.assertEqual(len(suite.CASES), 8)
        closure = json.loads((suite.FIXTURES/'dependencies.json').read_text())
        for case in suite.CASES:
            name, source = suite.case_source(case)
            base = suite.staged_source(name)
            self.assertEqual(base.split(b'WorldBegin')[1], source.split(b'WorldBegin')[1])
            self.assertEqual(base.split(b'Camera ')[1].split(b'Film ')[0],
                             source.split(b'Camera ')[1].split(b'Film ')[0])
            self.assertIn(b'PixelFilter "box" "float xradius" [1.5] "float yradius" [0.75]', source)
            self.assertIn(b'"integer pixelsamples" [4096]', source)
            self.assertIn(b'"integer seed" [0]', source)
            self.assertEqual(b'Sampler "sobol"' in source, case.endswith('sobol-fastowen'))
            with tempfile.TemporaryDirectory() as tmp:
                suite.comparison.stage_dependencies(suite.FIXTURES/(name+'.pbrt'), Path(tmp))
                for entry in closure[name]:
                    data = (Path(tmp)/entry['path']).read_bytes()
                    self.assertEqual(len(data), entry['bytes'])
                    self.assertEqual(hashlib.sha256(data).hexdigest(), entry['sha256'])
        with self.assertRaises(ValueError):
            suite.case_source('../outside')

    def test_statistical_gates(self):
        values = {backend: {region: [[1., 2., 3.]]*16 for region in suite.comparison.QUADRANTS}
                  for backend in ('rypath', 'pbrt')}
        gates = suite.statistics_record(values)
        self.assertEqual(len(gates), 15)
        self.assertTrue(all(g['passed'] for g in gates))
        changed = copy.deepcopy(values)
        changed['rypath']['full'][0][0] = 1000
        self.assertFalse(all(g['passed'] for g in suite.statistics_record(changed)))
        values['rypath']['full'].pop()
        with self.assertRaises(ValueError):
            suite.statistics_record(values)


if __name__ == '__main__':
    unittest.main()
