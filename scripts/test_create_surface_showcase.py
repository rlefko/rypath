#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate owned sphere topology, deterministic studios, and table provenance."""
import json
import math
from pathlib import Path
import re
import tempfile
import unittest

from create_surface_showcase import NAMES, generate, sphere


class SurfaceShowcaseTests(unittest.TestCase):
    def test_sphere_has_outward_nondegenerate_triangles_and_smooth_normals(self):
        source = sphere(0, 0, 0)
        def values(name, width, convert=float):
            data = [convert(v) for v in re.search('"'+name+'" \\[([^]]*)\\]', source)[1].split()]
            return [data[i:i+width] for i in range(0, len(data), width)]
        points, normals = values('point3 P', 3), values('normal N', 3)
        self.assertEqual(len(points), len(normals))
        for normal in normals:
            self.assertAlmostEqual(sum(v*v for v in normal), 1, places=7)
        for face in values('integer indices', 3, int):
            a, b, c = [points[i] for i in face]
            u, v = [[p[k]-a[k] for k in range(3)] for p in (b, c)]
            n = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
            self.assertGreater(sum(n[k]*a[k] for k in range(3)), 1e-6)
            self.assertTrue(all(math.isfinite(x) for p in (a,b,c) for x in p))

    def test_defaults_and_synthetic_inputs_are_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            metadata = generate(root)
            self.assertEqual(metadata['resolution'], [640, 400])
            self.assertEqual((metadata['spp'], metadata['seed']), (512, 42))
            self.assertEqual(set(metadata['labels']), set(NAMES))
            for name in NAMES:
                text = (root/(name+'.pbrt')).read_text()
                self.assertIn('"integer seed" 42', text)
                self.assertEqual(len(metadata['labels'][name]), 3)
            for asset in metadata['tables'].values():
                self.assertIn('not a physical measurement', asset['provenance'])
                self.assertEqual(len(asset['sha256']), 64)
            first = {name: (root/(name+'.pbrt')).read_bytes() for name in NAMES}
            generate(root)
            self.assertEqual(first, {name: (root/(name+'.pbrt')).read_bytes() for name in NAMES})
            self.assertEqual(json.loads((root/'showcase.json').read_text()), metadata)

    def test_caller_tables_remain_unmodified_and_missing_inputs_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = root/'asset.tensor'
            asset.write_bytes(b'caller-owned')
            metadata = generate(root/'output', measured=asset)
            self.assertEqual(asset.read_bytes(), b'caller-owned')
            self.assertEqual(metadata['tables']['measured']['path'], str(asset.resolve()))
            self.assertIn('retain its original license', metadata['tables']['measured']['provenance'])
            with self.assertRaises(ValueError):
                generate(root/'missing', fourier=root/'absent.bsdf')


if __name__ == '__main__':
    unittest.main()
