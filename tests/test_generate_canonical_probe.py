#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""CPU contracts for the test-only canonical probe compilation boundary."""

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import generate_canonical_probe as generator

ROOT = Path(__file__).resolve().parents[1]


class CanonicalProbeGenerationTests(unittest.TestCase):
    def test_exact_production_transform_is_reversible(self):
        render = (ROOT / "shaders/render.metal").read_bytes()
        probe = (ROOT / "shaders/light_geometry_sampling_probe.metal").read_bytes()
        transformed_render, transformed_probe = generator.transform(render, probe)
        self.assertEqual(transformed_render.replace(generator.OMITTED, generator.GUIDES), render)
        self.assertEqual(transformed_probe.replace(generator.ISOLATED, generator.RENDER), probe)
        self.assertEqual(transformed_render.count(b"\n"), render.count(b"\n"))
        self.assertEqual(transformed_probe.count(b"\n"), probe.count(b"\n"))
        for include in (b'#include "wavefront.metal"', b'#include "area_reuse.metal"',
                        b'#include "camera_medium.metal"'):
            self.assertIn(include, transformed_render)
        # Every probe body, including real MIS calls and material-retention
        # assertions' shader outputs, is byte-identical after the include line.
        self.assertEqual(transformed_probe.split(generator.ISOLATED)[1],
                         probe.split(generator.RENDER)[1])

    def test_changed_or_ambiguous_layout_fails(self):
        for render, probe in ((b"", generator.RENDER),
                              (generator.GUIDES * 2, generator.RENDER),
                              (generator.GUIDES, b""),
                              (generator.GUIDES, generator.RENDER * 2),
                              (generator.GUIDES + generator.OMITTED, generator.RENDER),
                              (generator.GUIDES, generator.RENDER + generator.ISOLATED)):
            with self.subTest(render=render, probe=probe), self.assertRaises(ValueError):
                generator.transform(render, probe)

    def test_real_files_and_hash_manifest(self):
        render = ROOT / "shaders/render.metal"
        probe = ROOT / "shaders/light_geometry_sampling_probe.metal"
        before = {p: p.read_bytes() for p in (render, probe)}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "derived"
            generator.generate(render, probe, output)
            record = json.loads((output / "generation.json").read_text())
            for name, digest in record["generated_files"].items():
                self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), digest)
            for path, digest in record["source_files"].items():
                self.assertEqual(hashlib.sha256(Path(path).read_bytes()).hexdigest(), digest)
            self.assertFalse(record["numeric_or_capability_policy_changes"])
        self.assertEqual({p: p.read_bytes() for p in before}, before)

    def test_invalid_layout_creates_no_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            render, probe, output = root / "render", root / "probe", root / "derived"
            render.write_bytes(b"changed include layout\n")
            probe.write_bytes(generator.RENDER)
            with self.assertRaises(ValueError):
                generator.generate(render, probe, output)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
