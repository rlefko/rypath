#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

from compare_pbrt_indexed import (
    external_case, generated_case, parse_u256, read_probe, require_equal_mips, rgb8_png,
    synthetic_cases,
)


def png_chunks(data):
    offset = 8  # PNG signature
    while offset < len(data):
        size, name = struct.unpack_from(">I4s", data, offset)
        yield name, data[offset + 8:offset + 8 + size]
        offset += size + 12


class PNGIndexedReferenceTests(unittest.TestCase):
    def test_odd_width_rows_use_msb_first_indices_and_keep_nonblack_transparent_rgb(self):
        palette = [(2, 4, 8), (16, 32, 64), (48, 96, 192)]
        case = generated_case("packed", 3, 2, 2, palette, [0, 1, 2, 2, 0, 1], b"\x00\x3d")
        chunks = dict(png_chunks(case.indexed))
        self.assertEqual(struct.unpack(">IIBBBBB", chunks[b"IHDR"]), (3, 2, 2, 3, 0, 0, 0))
        self.assertEqual(chunks[b"PLTE"], bytes.fromhex("020408 102040 3060c0"))
        self.assertEqual(chunks[b"tRNS"], b"\x00\x3d")
        self.assertEqual(zlib.decompress(chunks[b"IDAT"]), bytes.fromhex("00 1b 00 87"))
        self.assertEqual(case.expected_rgb, bytes.fromhex("020408 102040 3060c0 3060c0 020408 102040"))

        expanded = dict(png_chunks(rgb8_png(*case.dimensions, case.expected_rgb)))
        self.assertEqual(struct.unpack(">IIBBBBB", expanded[b"IHDR"]), (3, 2, 8, 2, 0, 0, 0))
        self.assertEqual(set(expanded), {b"IHDR", b"IDAT", b"IEND"})
        self.assertEqual(zlib.decompress(expanded[b"IDAT"]),
                         bytes.fromhex("00 020408 102040 3060c0 00 3060c0 020408 102040"))

    def test_full_palette_exposes_every_channel_code(self):
        case = next(case for case in synthetic_cases() if case.name == "all-codes")
        for channel in range(3):
            self.assertEqual(set(case.expected_rgb[channel::3]), set(range(256)))

    def test_rejects_index_outside_short_palette(self):
        with self.assertRaisesRegex(ValueError, "dimensions or indices"):
            generated_case("bad-index", 1, 1, 4, [(2, 4, 8)], [1])

    def test_rejects_unpinned_external_input(self):
        with tempfile.TemporaryDirectory() as directory:
            candidate = Path(directory) / "pinned.png"
            candidate.write_bytes(b"different data")
            with self.assertRaisesRegex(ValueError, "expected SHA-256"):
                external_case(candidate, "0" * 64)

    def test_parser_requires_complete_u256_mip_output(self):
        good = "2 1\n0000 0001 0002 00fd 00fe 00ff\n1 1\n007e 0080 0081\n"
        sizes = [(2, 1), (1, 1)]
        self.assertEqual(parse_u256(good, sizes, "fixture"),
                         [[0, 1, 2, 253, 254, 255], [126, 128, 129]])
        for text in (good[:-7], good.replace("00ff", "0100"), good + "\n",
                     good.replace("00fd", "fd"), good.replace("2 1", "1 2")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_u256(text, sizes, "fixture")

    def test_probe_failures_and_invalid_codes_are_propagated(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            command = ("probe", "u256", "fixture.png", "sRGB", "clamp", [(1, 1)], "test", output)
            error = subprocess.CalledProcessError(1, "probe", output="1 1\n0000 0000 0000\n")
            with patch("compare_pbrt_indexed.subprocess.check_output", side_effect=error) as run:
                with self.assertRaises(subprocess.CalledProcessError):
                    read_probe(*command)
                run.assert_called_once_with(
                    ["probe", "sRGB", "clamp", "fixture.png", "u256"], text=True, timeout=30
                )
            with patch("compare_pbrt_indexed.subprocess.check_output",
                       return_value="1 1\n0000 0000 0100\n"):
                with self.assertRaisesRegex(ValueError, "larger than 00ff"):
                    read_probe(*command)
            self.assertEqual(list(output.iterdir()), [])

    def test_mip_difference_reports_first_texel_channel(self):
        with self.assertRaisesRegex(ValueError, r"fixture mip 1: \(0, 0\) G expected 0007, got 0008"):
            require_equal_mips([[0] * 6, [0, 7, 0]], [[0] * 6, [0, 8, 0]],
                               [(2, 1), (1, 1)], "fixture")


if __name__ == "__main__":
    unittest.main()
