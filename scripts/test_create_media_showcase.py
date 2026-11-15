#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check deterministic medium studios, explicit nesting, and preset provenance."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from create_media_showcase import NAMES, generate, scene


class MediumShowcaseTests(unittest.TestCase):
    def test_defaults_content_hashes_and_determinism(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            metadata = generate(root)
            self.assertEqual(metadata['resolution'], [640, 400])
            self.assertEqual((metadata['spp'], metadata['seed']), (512, 42))
            self.assertEqual(set(metadata['labels']), set(NAMES))
            first = {}
            for name in NAMES:
                payload = (root/(name+'.pbrt')).read_bytes()
                first[name] = payload
                self.assertEqual(hashlib.sha256(payload).hexdigest(), metadata['files'][name]['sha256'])
                for token in (b'Integrator "volpath"', b'"integer seed" 42', b'MediumInterface',
                              b'"integer pixelsamples" 512'):
                    self.assertIn(token, payload)
                self.assertEqual(len(metadata['labels'][name]), 3)
            self.assertIn(b'MediumInterface "core" "medium2"', first['emission'])
            self.assertIn(b'Material "dielectric"', first['absorption'])
            self.assertEqual(generate(root), metadata)
            self.assertEqual(first, {name: (root/(name+'.pbrt')).read_bytes() for name in NAMES})
            self.assertEqual(json.loads((root/'showcase.json').read_text()), metadata)
            self.assertEqual(metadata['presets']['Apple']['sigma_s'], [2.29, 2.39, 1.97])
            self.assertIn('not original external', metadata['coverage'])

    def test_nondefault_settings_and_invalid_arguments(self):
        source, _ = scene('scattering', 64, 32, 8)
        for token in ('"integer xresolution" 64', '"integer yresolution" 32', '"integer pixelsamples" 8'):
            self.assertIn(token, source)
        for args in (('unknown',), ('emission', 0, 32, 8), ('emission', 32, 32, -1)):
            with self.assertRaises(ValueError):
                scene(*args)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                generate(Path(directory), height=0)


if __name__ == '__main__':
    unittest.main()
