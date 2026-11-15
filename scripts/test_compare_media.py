#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check source immutability, owned provenance, and independent-seed gates."""
import copy
import json
import math
import unittest

from compare_media import FIXTURES, SCENES, staged_source, statistics_record


class MediumComparisonTests(unittest.TestCase):
    def test_all_eight_owned_sources_have_matched_settings_and_no_dependencies(self):
        self.assertEqual(len(SCENES), 8)
        closure = json.loads((FIXTURES/'dependencies.json').read_text())
        for name in SCENES:
            source = staged_source(name)
            self.assertEqual(source, (FIXTURES/(name+'.pbrt')).read_bytes())
            for token in (b'"integer pixelsamples" [4096]', b'"integer seed" [0]',
                          b'"string sensor" "cie1931"', b'PixelFilter "box"', b'Integrator "volpath"'):
                self.assertIn(token, source)
            self.assertEqual(closure[name], [])
        with self.assertRaises(ValueError):
            staged_source('../other')
        provenance = json.loads((FIXTURES/'provenance.json').read_text())
        self.assertEqual(provenance['preset']['sigma_s_rgb'], [2.29, 2.39, 1.97])
        self.assertIn('not external scene-corpus conformance', provenance['coverage'])

    def test_gate_counts_seed_means_and_both_variances(self):
        values = [(1, 2, 3), (1.1, 2.1, 3.1), (.9, 1.9, 2.9), (1, 2, 3)]*2
        data = {'rypath': {'full': values}, 'pbrt': {'full': values}}
        records = statistics_record(data)
        self.assertTrue(all(r['passed'] and r['combined_se'] > 0 for r in records))
        expected = math.sqrt(2*(.04/7)/8)
        self.assertAlmostEqual(records[0]['combined_se'], expected)
        data['rypath']['full'] = [(r+1, g+1, b+1) for r, g, b in values]
        self.assertTrue(all(not r['passed'] for r in statistics_record(data)))

    def test_nonfinite_missing_channels_and_undersized_sets_fail(self):
        data = {'rypath': {'full': [(1, 2, 3)]*8}, 'pbrt': {'full': [(1, 2, 3)]*8}}
        for replacement in ([(float('nan'), 2, 3)]*8, [(1, 2)]*8, [(1, 2, 3)]*7):
            broken = copy.deepcopy(data)
            broken['rypath']['full'] = replacement
            with self.assertRaises(ValueError):
                statistics_record(broken)
        broken = copy.deepcopy(data)
        broken['pbrt'] = {}
        with self.assertRaises(ValueError):
            statistics_record(broken)


if __name__ == '__main__':
    unittest.main()
