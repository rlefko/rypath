#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check explicit, identical conductor reference variants without a GPU."""
import unittest
from pathlib import Path

from compare_conductors import comparison_variant


class VariantTests(unittest.TestCase):
    def test_positive_floor_changes_only_declared_endpoint_and_retains_native_fixture(self):
        path = Path(__file__).resolve().parents[1]/'tests/fixtures/pbrt-render/conductor/roughness-graph.pbrt'
        original = path.read_bytes()
        variant, overrides = comparison_variant('roughness-graph', original)
        self.assertEqual(variant, original.replace(b'"float tex1" [0]', b'"float tex1" [0.01]', 1))
        self.assertEqual(len(overrides), 1)
        self.assertEqual(overrides[0]['original'], '"float tex1" [0]')
        self.assertEqual(overrides[0]['replacement'], '"float tex1" [0.01]')
        self.assertIn(b'"float tex2" [0.15]', variant)
        self.assertIn(b'"integer pixelsamples" [4096]', variant)
        self.assertIn(b'"integer seed" [0]', variant)
        self.assertEqual(path.read_bytes(), original)

    def test_other_fixtures_are_unchanged_and_missing_or_duplicate_endpoint_fails(self):
        source = b'Material "conductor" "float roughness" [0]\n'
        self.assertEqual(comparison_variant('smooth-cu-environment', source), (source, []))
        for invalid in (source, b'"float tex1" [0]\n"float tex1" [0]'):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, 'expected one'):
                comparison_variant('roughness-graph', invalid)


if __name__ == '__main__':
    unittest.main()
