#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Guard reference evidence against silent nonfinite comparisons."""
import unittest

from compare_medium_presets import parse


class PresetEvidenceTests(unittest.TestCase):
    def test_valid_zero_and_hdr(self):
        self.assertEqual(parse('Medium name 00000000 7f7fffff\n', 2),
                         {'Medium name': ['00000000', '7f7fffff']})

    def test_nonfinite_and_negative(self):
        for bits in ('7fc00000', 'ffc00000', '7f800000', 'ff800000', 'bf800000'):
            with self.subTest(bits=bits), self.assertRaises(ValueError):
                parse(f'Medium {bits}\n', 1)

    def test_inventory_syntax(self):
        for text in ('Medium 0000000\n', 'Medium xyzxyzxy\n',
                     'Medium 00000000\nMedium 00000000\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse(text, 1)


if __name__ == '__main__':
    unittest.main()
