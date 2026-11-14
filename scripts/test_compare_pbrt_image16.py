#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

import struct
import unittest
import zlib
from pathlib import Path

from compare_pbrt_image16 import (
    AUTHORED_PIXELS, half_value, linear_half, mip_dimensions, parse_mips, png16,
    reduction_expected, require_equal, resize_x2,
)


class PNG16ReferenceTests(unittest.TestCase):
    def test_authored_input_preserves_distinct_rgb_low_bytes(self):
        assets = Path(__file__).resolve().parents[1] / "tests/fixtures/pbrt-render/image-assets"
        data = png16(4, 2, 2, AUTHORED_PIXELS)
        self.assertEqual(data, (assets / "asymmetric16.png").read_bytes())
        self.assertEqual(struct.unpack_from(">IIBB", data, 16), (4, 2, 16, 2))
        size = struct.unpack_from(">I", data, 33)[0]
        self.assertEqual(data[37:41], b"IDAT")
        rows = zlib.decompress(data[41:41 + size])
        self.assertEqual(rows[0], 0)
        self.assertEqual(rows[25], 0)
        self.assertEqual(rows[1:13], bytes.fromhex("0101 03f8 0bf2 01fe 03fe 0b02"))

    def test_selected_mips_distinguish_working_and_stored_rounding(self):
        self.assertEqual(reduction_expected(), ([0x0100, 0x0100, 0x1BF8, 0x2C8D],
                                               [0x0100, 0x28CD], [0x24CD]))
        self.assertEqual(list(mip_dimensions(3, 1)), [(4, 1), (2, 1), (1, 1)])
        self.assertEqual(list(mip_dimensions(1, 3)), [(1, 4), (1, 2), (1, 1)])

    def test_npot_clamp_and_repeat_overshoot_and_source_rounding(self):
        self.assertEqual(resize_x2((0, 1, 1, 1)), 0x3C53)
        self.assertEqual(resize_x2((0, 1, 1, 0)), 0x3C7B)
        source = (1, 0x8888, 1, 1)
        self.assertEqual(resize_x2([half_value(linear_half(code)) for code in source]), 0x3635)
        self.assertEqual(resize_x2([code / 65535 for code in source]), 0x3636)

    def test_rejects_truncated_or_invalid_mip_output(self):
        good = "2 1\n0000 0001 0002 3c00 3c01 3c02\n1 1\n3800 3801 3802\n"
        dimensions = [(2, 1), (1, 1)]
        levels = parse_mips(good, dimensions, "fixture")
        self.assertEqual(levels, [[0, 1, 2, 0x3C00, 0x3C01, 0x3C02], [0x3800, 0x3801, 0x3802]])
        for output in (good[:-5], good + "extra\n", good.replace("2 1", "1 2"),
                       good.replace("0000", "7e00"), good.replace("3c00", "3c0x"),
                       good.replace("0001", "0001 0001")):
            with self.subTest(output=output), self.assertRaises(ValueError):
                parse_mips(output, dimensions, "fixture")

    def test_difference_reports_texel_and_channel(self):
        with self.assertRaisesRegex(ValueError, r"mip 0: \(1, 0\) G expected 0007, got 0008"):
            require_equal([0, 0, 0, 0, 7, 0], [0, 0, 0, 0, 8, 0], 2, "mip 0")


if __name__ == "__main__":
    unittest.main()
