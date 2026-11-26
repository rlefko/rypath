#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""CPU contracts for the production-bound arithmetic compilation boundaries."""

import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import generate_canonical_probe as generator

ROOT = Path(__file__).resolve().parents[1]


class CanonicalProbeGenerationTests(unittest.TestCase):
    def setUp(self):
        self.render = (ROOT / 'shaders/render.metal').read_bytes()
        self.probe = (ROOT / 'shaders/light_geometry_sampling_probe.metal').read_bytes()
        self.transport = (ROOT / 'shaders/packet_transport.h').read_bytes()

    def test_exact_production_transform_is_reversible(self):
        (render, probe, transport, render_edits, probe_edits,
         transport_edits) = generator.transform_details(self.render, self.probe, self.transport)
        for original, derived, edits in ((self.render, render, render_edits),
                                         (self.probe, probe, probe_edits),
                                         (self.transport, transport, transport_edits)):
            self.assertEqual(original.count(b'\n'), derived.count(b'\n'))
            for edit in reversed(edits):
                derived = derived.replace(edit['replacement'], edit['original'], 1)
            self.assertEqual(derived, original)
        self.assertEqual(generator.kernels(render), [])
        self.assertEqual(tuple(row[0] for row in generator.kernels(probe)), generator.PROBE_KERNELS)
        self.assertEqual(len(render_edits), 15)
        self.assertEqual(len(transport_edits), 1)
        self.assertEqual(len(probe_edits), 2)
        # All five executed kernel bodies remain exact production source slices.
        original_rows = {name: self.probe[begin:end]
                         for name, begin, end in generator.kernels(self.probe)}
        for name, begin, end in generator.kernels(probe):
            self.assertEqual(probe[begin:end], original_rows[name])
        self.assertIn(generator.ISOLATED_TRANSPORT, render)
        self.assertIn(b'#include "scalar_transport.h"', render)
        self.assertNotIn(generator.TRANSPORT, render)
        self.assertNotIn(generator.PT_STEP, transport)
        # Every transport helper byte is retained; the sole omission is the
        # original final include, with a reversible line-preserving marker.
        self.assertTrue(transport.startswith(self.transport[:self.transport.index(generator.PT_STEP)]))
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
                generator.transform(render, probe, self.transport)

    def test_transport_boundary_drift_fails(self):
        variants = [self.transport.replace(generator.PT_STEP, b''),
                    self.transport + generator.PT_STEP,
                    self.transport + b'// new text after reviewed boundary\n',
                    self.transport.replace(generator.PT_STEP, b'#include "renamed_step.h"\n'),
                    self.transport + b'kernel void hidden() {}\n']
        for transport in variants:
            with self.subTest(transport=transport[-80:]), self.assertRaises(ValueError):
                generator.transform(self.render, self.probe, transport)
        for render in (self.render.replace(generator.TRANSPORT, b''),
                       self.render.replace(generator.TRANSPORT, generator.TRANSPORT * 2),
                       self.render + generator.ISOLATED_TRANSPORT):
            with self.subTest(render=render[-80:]), self.assertRaises(ValueError):
                generator.transform(render, self.probe, self.transport)

    def test_transitive_pt_reintroduction_fails_before_outputs(self):
        render = ROOT / 'shaders/render.metal'
        probe = ROOT / 'shaders/light_geometry_sampling_probe.metal'
        target = (ROOT / 'shaders/packet_scattering.h').resolve()
        original_read = Path.read_bytes

        def reintroduce(path):
            data = original_read(path)
            return data + generator.PT_STEP if path.resolve() == target else data

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'derived'
            with mock.patch.object(Path, 'read_bytes', reintroduce):
                with self.assertRaisesRegex(ValueError, 'PT-step escaped'):
                    generator.generate(render, probe, output)
            self.assertFalse(output.exists())

    def test_real_files_and_hash_manifest(self):
        render = ROOT / 'shaders/render.metal'
        probe = ROOT / 'shaders/light_geometry_sampling_probe.metal'
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'derived'
            generator.generate(render, probe, output)
            record = json.loads((output / 'generation.json').read_text())
            self.assertEqual(record['schema'], 3)
            self.assertEqual(record['entrypoints'], list(generator.PROBE_KERNELS))
            for name, digest in record['generated_files'].items():
                self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), digest)
            for field in ('source_files', 'retained_helper_files'):
                self.assertTrue(record[field])
                for path, digest in record[field].items():
                    self.assertEqual(hashlib.sha256(Path(path).read_bytes()).hexdigest(), digest)
            self.assertIn(str((ROOT / 'shaders/packet_transport.h').resolve()), record['source_files'])
            self.assertNotIn(str((ROOT / 'shaders/packet_pt_step.h').resolve()),
                             record['retained_helper_files'])
            self.assertNotIn(str((ROOT / 'shaders/hero_current_bsdf.h').resolve()),
                             record['retained_helper_files'])
            self.assertIn('canonical_packet_transport.h', record['generated_files'])
            self.assertEqual(len(record['edits']['transport']), 1)
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


class CurrentProbeGenerationTests(unittest.TestCase):
    def setUp(self):
        self.paths = (ROOT / 'shaders/render.metal', ROOT / 'tests/hero_current_probe.metal',
                      ROOT / 'shaders/packet_transport.h', ROOT / 'shaders/packet_pt_step.h')
        self.sources = tuple(p.read_bytes() for p in self.paths)

    def test_exact_current_sources_and_prefix_are_reversible(self):
        result = generator.transform_current_details(*self.sources)
        for original, derived, edits in zip(self.sources, result[:4], result[4:]):
            self.assertEqual(original.count(b'\n'), derived.count(b'\n'))
            for edit in reversed(edits):
                derived = derived.replace(edit['replacement'], edit['original'], 1)
            self.assertEqual(derived, original)
        self.assertEqual(generator.kernels(result[0]), [])
        original_rows = {name: self.sources[1][begin:end]
                         for name, begin, end in generator.kernels(self.sources[1])}
        self.assertEqual(tuple(original_rows), generator.CURRENT_KERNELS)
        for name, begin, end in generator.kernels(result[1]):
            self.assertEqual(result[1][begin:end], original_rows[name])
        end = self.sources[3].index(generator.STATE_END) + len(generator.STATE_END)
        self.assertTrue(result[3].startswith(self.sources[3][:end]))
        self.assertNotIn(b'packet_pt_initialize(', result[3])
        self.assertNotIn(b'packet_pt_event_body.inc', result[3])
        self.assertIn(generator.CURRENT_STATE, result[2])

    def test_current_boundary_drift_fails(self):
        render, probe, transport, step = self.sources
        variants = [(render, probe, transport, step.replace(generator.STATE_END, b'')),
                    (render, probe, transport, step + generator.STATE_END),
                    (render, probe, transport, step.replace(generator.STATE_NEXT, b'renamed(')),
                    (render, probe, transport, step.replace(
                        generator.STATE_END, b'inline void packet_pt_trace() {}\n' + generator.STATE_END)),
                    (render, probe, transport, step.replace(
                        generator.STATE_END, b'#include "packet_pt_event_body.inc"\n' + generator.STATE_END)),
                    (render, probe + b'kernel void hidden() {}\n', transport, step),
                    (render, probe.replace(generator.CURRENT_PROBE_RENDER, b''), transport, step),
                    (render, probe, transport + generator.PT_STEP, step)]
        for sources in variants:
            with self.subTest(lengths=tuple(map(len, sources))), self.assertRaises(ValueError):
                generator.transform_current_details(*sources)

    def test_current_manifest_and_retained_material_state_closure(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'derived'
            generator.generate_current(self.paths[0], self.paths[1], output)
            record = json.loads((output / 'generation.json').read_text())
            self.assertEqual(record['entrypoints'], list(generator.CURRENT_KERNELS))
            self.assertEqual(len(record['source_files']), 4)
            for name, digest in record['generated_files'].items():
                self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), digest)
            for field in ('source_files', 'retained_helper_files'):
                for path, digest in record[field].items():
                    self.assertEqual(hashlib.sha256(Path(path).read_bytes()).hexdigest(), digest)
            closure = {Path(p).name for p in record['retained_helper_files']}
            self.assertIn('hero_current_bsdf.h', closure)
            self.assertIn('hero_frozen_bsdf.h', closure)
            self.assertIn('hero_pt_state.h', closure)
            self.assertNotIn('packet_pt_step.h', closure)
            self.assertNotIn('packet_pt_event_body.inc', closure)
            self.assertFalse(record['numeric_or_capability_policy_changes'])
        self.assertEqual(tuple(p.read_bytes() for p in self.paths), self.sources)


if __name__ == '__main__':
    unittest.main()
