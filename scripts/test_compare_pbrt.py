#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

import contextlib
import hashlib
import io
from pathlib import Path
import re
import struct
import tempfile
import unittest

from compare_pbrt import QUADRANTS, REGIONS, compare_regions, read_pfm, roi_means


CORNELL_CREDIT = (b"# SPDX-License-Identifier: CC0-1.0\n"
                  b"# Cornell Box by Benedikt Bitterli, converted to PBRT v4 by Matt Pharr.\n"
                  b"# Adapted for comparison in rypath. See ../pbrt/README.md and ../pbrt/LICENSE.txt.\n")
CORNELL_FILM_EDIT = (
    b'"integer yresolution" [ 1024 ]\n    "integer xresolution" [ 1024 ]',
    b'"integer yresolution" [8]\n    "integer xresolution" [8]\n'
    b'    "string sensor" "cie1931"\n    "float iso" [100]\n    "bool savefp16" [false]',
)


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
    def test_triangle_filter_compares_full_frame_and_quadrants(self):
        self.assertEqual(REGIONS["triangle-filter"], {
            "full": (0, 0, 8, 8),
            "top-left": (0, 0, 4, 4),
            "top-right": (4, 0, 8, 4),
            "bottom-left": (0, 4, 4, 8),
            "bottom-right": (4, 4, 8, 8),
        })

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


class CornellFixtureTests(unittest.TestCase):
    def test_derivative_uses_only_documented_edits(self):
        fixtures = Path(__file__).resolve().parents[1] / "tests" / "fixtures"
        original = (fixtures / "pbrt" / "cornell-box.pbrt").read_bytes()
        notice = (fixtures / "pbrt" / "LICENSE.txt").read_bytes()
        self.assertEqual(hashlib.sha256(original).hexdigest(),
                         "b2fc6654c966468119187d169a76ecec82e169a1058f3344e27e5103189d4e43")
        self.assertEqual(hashlib.sha256(notice).hexdigest(),
                         "520b18c04d27fd9abca81364746178a9a3d52cff3c6204de05be5b68e547ee43")

        expected = original
        for old, new in (
            (b'Sampler "sobol"\n    "integer pixelsamples" [ 64 ]',
             b'Sampler "independent"\n    "integer pixelsamples" [4096]\n    "integer seed" [0]'),
            (b'PixelFilter "triangle"\n    "float xradius" [ 1 ]\n    "float yradius" [ 1 ]',
             b'PixelFilter "box"\n    "float xradius" [0.5]\n    "float yradius" [0.5]'),
            CORNELL_FILM_EDIT,
        ):
            self.assertEqual(expected.count(old), 1)
            expected = expected.replace(old, new, 1)
        for parameter in (b"normal N", b"point2 uv"):
            pattern = rb'(?m)^[ \t]+"' + parameter + rb'"\s*\[[^\]]*\]\n'
            expected, removed = re.subn(pattern, b"", expected)
            self.assertEqual(removed, 8)
        adapted = (fixtures / "pbrt-render" / "cornell-box-adapted.pbrt").read_bytes()
        self.assertEqual(adapted, CORNELL_CREDIT + expected)

    def test_sobol_derivatives_preserve_the_original_scene(self):
        fixtures = Path(__file__).resolve().parents[1] / "tests" / "fixtures"
        original = (fixtures / "pbrt" / "cornell-box.pbrt").read_bytes()
        film_token = CORNELL_FILM_EDIT[0]
        samples_token = b'"integer pixelsamples" [ 64 ]'
        self.assertEqual(original.count(film_token), 1)
        self.assertEqual(original.count(samples_token), 1)
        base = original.replace(*CORNELL_FILM_EDIT, 1)
        for mode in ("fastowen", "permutedigits", "owen"):
            with self.subTest(mode=mode):
                scene = f"cornell-box-sobol-{mode}"
                adapted = (fixtures / "pbrt-render" / f"{scene}.pbrt").read_bytes()
                expected = base.replace(
                    samples_token,
                    b'"integer pixelsamples" [4096]\n    "integer seed" [0]\n'
                    + f'    "string randomization" "{mode}"'.encode(),
                    1,
                )
                self.assertEqual(adapted, CORNELL_CREDIT + expected)
                self.assertEqual(REGIONS[scene], QUADRANTS)

    def test_all_area_fixtures_compare_full_frame_and_quadrants(self):
        for scene in ("area-visible", "area-direct", "area-mixed", "cornell-box-adapted"):
            with self.subTest(scene=scene):
                self.assertEqual(REGIONS[scene], {
                    "full": (0, 0, 8, 8),
                    "top-left": (0, 0, 4, 4),
                    "top-right": (4, 0, 8, 4),
                    "bottom-left": (0, 4, 4, 8),
                    "bottom-right": (4, 4, 8, 8),
                })


if __name__ == "__main__":
    unittest.main()
