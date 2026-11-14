#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from compare_pbrt_grayscale import external_case, generated_case, synthetic_cases


class PNGGrayscaleReferenceTests(unittest.TestCase):
    def test_odd_width_rows_pack_samples_and_keep_keyed_gray(self):
        case = generated_case("packed", 3, 2, 2, [0, 1, 2, 3, 0, 1], key=2, split_idat=True)
        offset = 8  # PNG signature
        chunks = []
        while offset < len(case.grayscale):
            size, name = struct.unpack_from(">I4s", case.grayscale, offset)
            chunks.append((name, case.grayscale[offset + 8:offset + 8 + size]))
            offset += size + 12
        self.assertEqual([name for name, _ in chunks],
                         [b"IHDR", b"gAMA", b"tRNS", b"IDAT", b"IDAT", b"IDAT", b"IEND"])
        self.assertEqual(struct.unpack(">IIBBBBB", chunks[0][1]), (3, 2, 2, 0, 0, 0, 0))
        self.assertEqual(chunks[2][1], b"\x00\x02")
        payload = b"".join(data for name, data in chunks if name == b"IDAT")
        self.assertEqual(zlib.decompress(payload), bytes.fromhex("00 1b 00 c7"))
        self.assertEqual(case.expected_rgb, bytes.fromhex("000000 555555 aaaaaa ffffff 000000 555555"))

    def test_odd_width_cases_expand_the_entire_source_domain(self):
        cases = {case.name: case for case in synthetic_cases()}
        for depth in (1, 2, 4):
            odd = cases[f"odd-{depth}bit"]
            expected = {sample * 255 // ((1 << depth) - 1) for sample in range(1 << depth)}
            for channel in range(3):
                self.assertEqual(set(odd.expected_rgb[channel::3]), expected)
            self.assertFalse(odd.compare_pbrt_mips)
            self.assertTrue(cases[f"pot-{depth}bit"].compare_pbrt_mips)

    def test_rejects_unpinned_asset_before_decoding(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.png"
            path.write_bytes(b"unpinned")
            with self.assertRaisesRegex(ValueError, "expected SHA-256"):
                external_case(path)


if __name__ == "__main__":
    unittest.main()
