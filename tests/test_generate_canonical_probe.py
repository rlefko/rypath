#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""CPU contracts for the five-entrypoint compilation boundary."""

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import generate_canonical_probe as generator

ROOT = Path(__file__).resolve().parents[1]


class CanonicalProbeGenerationTests(unittest.TestCase):
    def setUp(self):
        self.render = (ROOT / 'shaders/render.metal').read_bytes()
        self.probe = (ROOT / 'shaders/light_geometry_sampling_probe.metal').read_bytes()

    def test_exact_production_transform_is_reversible(self):
        render, probe, render_edits, probe_edits = generator.transform_details(self.render, self.probe)
        for original, derived, edits in ((self.render, render, render_edits),
                                         (self.probe, probe, probe_edits)):
            self.assertEqual(original.count(b'\n'), derived.count(b'\n'))
            for edit in reversed(edits):
                derived = derived.replace(edit['replacement'], edit['original'], 1)
            self.assertEqual(derived, original)
        self.assertEqual(generator.kernels(render), [])
        self.assertEqual(tuple(row[0] for row in generator.kernels(probe)), generator.PROBE_KERNELS)
        self.assertEqual(len(render_edits), 14)
        self.assertEqual(len(probe_edits), 2)
        # All five executed kernel bodies remain exact production source slices.
        original_rows = {name: self.probe[begin:end]
                         for name, begin, end in generator.kernels(self.probe)}
        for name, begin, end in generator.kernels(probe):
            self.assertEqual(probe[begin:end], original_rows[name])
        for include in (b'#include "packet_transport.h"', b'#include "scalar_transport.h"'):
            self.assertIn(include, render)
        self.assertNotIn(b'namespace strategy_context21', probe)

    def test_balanced_scanner_ignores_literals_and_comments(self):
        source = b'''// kernel void fake() {\n
kernel void real() { const char *s = "} /* kernel \\\"";
/* } kernel void fake2() { */ if (true) { /* { */ } // }
}
'''
        self.assertEqual([row[0] for row in generator.kernels(source)], ['real'])
        self.assertEqual(generator.kernels(source)[0][2], len(source))

    def test_scanner_rejects_malformed_layout(self):
        for source in (b'/* missing end', b'"missing end', b'kernel void x() {\n',
                       b'kernel void x();\n', b'kernel float x() {}\n',
                       b'kernel void x() {\nkernel void y() {}\n}\n'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                generator.kernels(source)

    def test_changed_or_ambiguous_inventory_fails(self):
        mutations = [
            (self.render.replace(b'kernel void rypath_render_sky', b'kernel void renamed'), self.probe),
            (self.render + b'kernel void added() {}\n', self.probe),
            (self.render + b'kernel void rypath_render_packets() {}\n', self.probe),
            (self.render.replace(b'#include "area_reuse.metal"\n', b''), self.probe),
            (self.render.replace(b'#include "area_reuse.metal"\n',
                                 b'#include "area_reuse.metal"\n' * 2), self.probe),
            (self.render + generator.PREFIX, self.probe),
            (self.render, self.probe.replace(generator.RENDER, b'')),
            (self.render, self.probe.replace(generator.RENDER, generator.RENDER * 2)),
            (self.render, self.probe.replace(generator.FULL_MARKER, b'')),
            (self.render, self.probe + b'kernel void unexpected() {}\n'),
            (self.render, self.probe + generator.ISOLATED),
        ]
        for render, probe in mutations:
            with self.subTest(render_len=len(render), probe_len=len(probe)), self.assertRaises(ValueError):
                generator.transform(render, probe)

    def test_real_files_and_hash_manifest(self):
        render = ROOT / 'shaders/render.metal'
        probe = ROOT / 'shaders/light_geometry_sampling_probe.metal'
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'derived'
            generator.generate(render, probe, output)
            record = json.loads((output / 'generation.json').read_text())
            self.assertEqual(record['schema'], 2)
            self.assertEqual(record['entrypoints'], list(generator.PROBE_KERNELS))
            for name, digest in record['generated_files'].items():
                self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), digest)
            for field in ('source_files', 'retained_helper_files'):
                self.assertTrue(record[field])
                for path, digest in record[field].items():
                    self.assertEqual(hashlib.sha256(Path(path).read_bytes()).hexdigest(), digest)
            self.assertFalse(record['numeric_or_capability_policy_changes'])
        self.assertEqual(render.read_bytes(), self.render)
        self.assertEqual(probe.read_bytes(), self.probe)

    def test_invalid_layout_creates_no_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            render, probe, output = root / 'render', root / 'probe', root / 'derived'
            render.write_bytes(b'changed layout\n')
            probe.write_bytes(generator.RENDER)
            with self.assertRaises(ValueError):
                generator.generate(render, probe, output)
            self.assertFalse(output.exists())
            output.mkdir()
            sentinel = output / 'generation.json'
            sentinel.write_bytes(b'existing output')
            with self.assertRaises(ValueError):
                generator.generate(render, probe, output)
            self.assertEqual(sentinel.read_bytes(), b'existing output')
            self.assertEqual(list(output.iterdir()), [sentinel])

    def test_helper_closure_rejects_hidden_entrypoint_or_missing_include(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            render, probe = root / 'render.metal', root / 'probe.metal'
            helper = root / 'helper.h'
            helper.write_bytes(b'kernel void hidden() {}\n')
            for source in (b'#include "helper.h"\n', b'#include "absent.h"\n',
                           b'#include UNKNOWN_HEADER\n'):
                with self.subTest(source=source), self.assertRaises(ValueError):
                    generator.helper_closure(render, probe, source, b'')
            self.assertEqual(generator.helper_closure(
                render, probe, b'/*\n#include "absent.h"\n*/\n', b''), {})


if __name__ == '__main__':
    unittest.main()
