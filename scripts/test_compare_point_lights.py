#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Point fixtures preserve source/dependency hashes and detect biased estimators."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from compare_point_lights import FIXTURES, SCENES, staged_source, statistics_record
from compare_pbrt import stage_dependencies


class PointComparisonTests(unittest.TestCase):
    def test_matched_sources_and_dependency_closure(self):
        self.assertEqual(len(SCENES), 8)
        closure = json.loads((FIXTURES/'dependencies.json').read_text())
        for name in SCENES:
            source = staged_source(name)
            for token in (b'LightSource "point"', b'"integer pixelsamples" [4096]',
                          b'"integer seed" [0]', b'"string sensor" "cie1931"', b'PixelFilter "box"'):
                self.assertIn(token, source)
            with tempfile.TemporaryDirectory() as tmp:
                stage_dependencies(FIXTURES/(name+'.pbrt'), Path(tmp))
                for record in closure[name]:
                    data = (Path(tmp)/record['path']).read_bytes()
                    self.assertEqual(len(data), record['bytes'])
                    self.assertEqual(hashlib.sha256(data).hexdigest(), record['sha256'])
        self.assertIn(b'Integrator "volpath"', staged_source('volume'))
        with self.assertRaises(ValueError):
            staged_source('../other')

    def test_external_asset_is_unchanged_with_retained_attribution(self):
        record = json.loads((FIXTURES/'provenance.json').read_text())['external-quad']
        self.assertEqual(record['license'], 'CC BY 2.0')
        self.assertTrue(record['unchanged'])
        self.assertEqual((FIXTURES/'assets/sportscar-quad.ply').read_bytes(),
                         (FIXTURES.parent/'conductor/assets/sportscar-quad.ply').read_bytes())

    def test_gate_and_negative_bias_control(self):
        values = [(1+.001*(i % 3), 2, 3) for i in range(16)]
        data = {'rypath': {'full': values}, 'pbrt': {'full': values}}
        self.assertTrue(all(r['passed'] for r in statistics_record(data)))
        # Missing point NEE, accidental MIS downweighting, or an inverse-square
        # error must be detected even when all output samples remain finite.
        for factor in (0, .5, 2):
            biased = copy.deepcopy(data)
            biased['rypath']['full'] = [tuple(factor*x for x in rgb) for rgb in values]
            self.assertTrue(all(not r['passed'] for r in statistics_record(biased)))
        for values in ([(float('nan'), 2, 3)]*16, [(1, 2)]*16, [(1, 2, 3)]*7):
            bad = copy.deepcopy(data)
            bad['rypath']['full'] = values
            with self.assertRaises(ValueError):
                statistics_record(bad)


if __name__ == '__main__':
    unittest.main()
