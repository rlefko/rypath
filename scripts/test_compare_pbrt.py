#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

import contextlib
import io
from pathlib import Path
import struct
import tempfile
import unittest

from compare_pbrt import compare_regions, read_pfm, roi_means


class PFMTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name) / "test.pfm"

    def test_rgb_range_and_row_order_in_both_byte_orders(self):
        # The two scanlines have distinct values. RGB uses a signed, unbounded range.
        top = (-2, 0.25, 4, 2, 3, 5)
        bottom = (8, -1, 3, 7, 6, 5)
        for scale, order in ((b"-1.0", "<"), (b"1.0", ">")):
            with self.subTest(scale=scale):
                self.path.write_bytes(b"PF\n2 2\n" + scale + b"\n" +
                                      struct.pack(order + "12f", *bottom, *top))
                self.assertEqual(
                    read_pfm(self.path, 2, 2),
                    [top[:3], top[3:], bottom[:3], bottom[3:]],
                )

    def test_rejects_malformed_pfm(self):
        payload = struct.pack("<3f", 0, 1, 2)
        for header, pixels in (
            (b"Pf\n1 1\n-1\n", payload),
            (b"PF\n99999999999999999 1\n-1\n", payload),
            (b"PF\n" + b" " * 100 + b"1 1\n-1\n", payload),
            (b"PF\n1 1\n2\n", payload),
            (b"PF\n1 1\nnan\n", payload),
            (b"PF\n1 1\n-1\n", payload[:-1]),
            (b"PF\n1 1\n-1\n", payload + b"\0"),
            (b"PF\n1 1\n-1\n", struct.pack("<3f", float("inf"), 0, 0)),
        ):
            with self.subTest(header=header, pixels=pixels):
                self.path.write_bytes(header + pixels)
                with self.assertRaises(ValueError):
                    read_pfm(self.path, 1, 1)


class ComparisonTests(unittest.TestCase):
    def test_rois_use_top_down_coordinates(self):
        pixels = [(x, y, 8 * y + x) for y in range(8) for x in range(8)]
        self.assertEqual(roi_means(pixels, (4, 0, 8, 4)), (5.5, 1.5, 17.5))
        self.assertEqual(roi_means(pixels, (0, 4, 4, 8)), (1.5, 5.5, 45.5))

    def test_channel_gate_includes_reference_relative_floor(self):
        reference = {"full": [(0, 0, 10)] * 16}
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(
                compare_regions("test", {"pbrt": reference, "rypath": {"full": [(0, 0, 10.0001)] * 16}})
            )
            self.assertFalse(
                compare_regions("test", {"pbrt": reference, "rypath": {"full": [(0, 0, 10.001)] * 16}})
            )


if __name__ == "__main__":
    unittest.main()
