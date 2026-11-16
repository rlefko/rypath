#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Distant reference inputs, execution provenance, and independent seed gates."""
import copy
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from compare_distant_lights import (FIXTURES, SCENES, REVISION, backend_sources,
                                    render_pfm, render_backend_sources, staged_source, statistics_record)
from compare_pbrt import stage_dependencies


class DistantComparisonTests(unittest.TestCase):
    def test_owned_fixture_protocol_and_dependency_closure(self):
        self.assertEqual(len(SCENES), 9)
        provenance = json.loads((FIXTURES/'provenance.json').read_text())
        self.assertEqual(provenance['pbrt_revision'], REVISION)
        self.assertEqual(provenance['reference_coordinate_system'], 'world')
        self.assertEqual(provenance['license'], 'GPL-3.0-only')
        self.assertEqual(len(provenance['external_assets']), 1)
        self.assertEqual(set(provenance['scenes']), set(SCENES))
        closure = json.loads((FIXTURES/'dependencies.json').read_text())
        for name in SCENES:
            source = staged_source(name)
            for token in (b'LightSource "distant"', b'"integer pixelsamples" [4096]',
                          b'"integer seed" [0]', b'"string sensor" "cie1931"',
                          b'PixelFilter "box"', b'"integer xresolution" [8]',
                          b'"integer yresolution" [8]'):
                self.assertIn(token, source)
            self.assertEqual(provenance['scenes'][name]['entry'], name+'.pbrt')
            with tempfile.TemporaryDirectory() as tmp:
                stage_dependencies(FIXTURES/(name+'.pbrt'), Path(tmp))
                for record in closure[name]:
                    data = (Path(tmp)/record['path']).read_bytes()
                    self.assertEqual(len(data), record['bytes'])
                    self.assertEqual(hashlib.sha256(data).hexdigest(), record['sha256'])
        with self.assertRaises(ValueError):
            staged_source('../other')

    def test_external_quad_terms_and_unchanged_asset(self):
        provenance = json.loads((FIXTURES/'provenance.json').read_text())
        record = provenance['external-quad']
        self.assertEqual(record['license'], 'CC BY 2.0')
        self.assertEqual(record['author'], 'Yasutoshi Mori')
        self.assertEqual(record['upstream_revision'], '30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572')
        self.assertTrue(record['unchanged'])
        self.assertEqual((FIXTURES/record['path']).read_bytes(),
                         (FIXTURES.parent/'conductor/assets/sportscar-quad.ply').read_bytes())

    def test_semantic_coverage(self):
        self.assertIn(b'"float illuminance" [850]', staged_source('spectral-illuminance'))
        affine = staged_source('affine-transform')
        self.assertIn(b'Transform [', affine)
        self.assertIn(b'ObjectInstance "receiver"', affine)
        for token in (b'LightSource "point"', b'LightSource "spot"', b'LightSource "infinite"'):
            self.assertIn(token, staged_source('mixed'))
        self.assertIn(b'Material "dielectric"', staged_source('rough-transmission'))
        bounded = staged_source('bounded-volume')
        self.assertIn(b'Material "interface"', bounded)
        self.assertIn(b'MediumInterface "fog" ""', bounded)
        unbounded = staged_source('unbounded-volume')
        # Active camera medium and nonempty finite geometry are both essential:
        # an empty bound or an infinite connection would erase distant signal.
        self.assertLess(unbounded.index(b'MediumInterface "" "fog"'),
                        unbounded.index(b'Camera "perspective"'))
        self.assertIn(b'Shape "trianglemesh"', unbounded)
        self.assertIn(b'"spectrum sigma_a" [360 .04 830 .04]', unbounded)
        self.assertNotIn(b'LightSource "infinite"', unbounded)
        for name in ('bounded-volume', 'unbounded-volume'):
            self.assertIn(b'Integrator "volpath"', staged_source(name))

    def test_backend_sources_and_explicit_world_override(self):
        for name in SCENES:
            source = staged_source(name)
            sources, overrides = backend_sources(name, source)
            self.assertEqual(sources, {'rypath': source, 'pbrt': source})
            self.assertEqual(overrides['rypath'], [])
            self.assertEqual(overrides['pbrt'][0]['parameter'], '--render-coord-sys')
            self.assertEqual(overrides['pbrt'][0]['comparison'], 'world')
            self.assertIn('2*sceneRadius', overrides['pbrt'][0]['reason'])

    def test_retained_command_has_world_override_only_for_pbrt(self):
        commands = []

        def runner(command, **kwargs):
            commands.append(command.copy())
            self.assertTrue(kwargs['capture_output'])
            self.assertEqual(kwargs['timeout'], 120)
            output = Path(command[command.index('--outfile')+1] if '--outfile' in command
                          else command[-1])
            output.write_bytes(b'PF\n8 8\n-1\n'+struct.pack('<192f', *([1.]*192)))
            return subprocess.CompletedProcess(command, 0, '', '')

        with tempfile.TemporaryDirectory() as tmp:
            for backend in ('rypath', 'pbrt'):
                variant = Path(tmp)/(backend+'.pbrt')
                pixels = render_pfm(backend, Path('/fake/renderer'), variant,
                                    staged_source('diffuse-direction'), runner=runner)
                self.assertEqual(len(pixels), 64)
                record = json.loads(variant.with_suffix('.process.json').read_text())
                self.assertEqual(record['command'], commands[-1])
                self.assertFalse(record['error_diagnostic'])
            self.assertNotIn('--render-coord-sys', commands[0])
            self.assertEqual(commands[1][1:5], ['--render-coord-sys', 'world', '--nthreads', '2'])

    def test_fixed_nonoverlapping_seed_schedule(self):
        seeds = {'rypath': [], 'pbrt': []}

        def render(backend, binary, variant, source):
            seeds[backend].append(int(re.search(rb'"integer seed" \[(\d+)\]', source)[1]))
            return [(1., 2., 3.)]*64

        source = staged_source('diffuse-direction')
        with tempfile.TemporaryDirectory() as tmp:
            with patch('compare_distant_lights.render_pfm', side_effect=render):
                result = render_backend_sources(Path('/fake/ours'), Path('/fake/pbrt'),
                    FIXTURES/'diffuse-direction.pbrt', Path(tmp),
                    {'rypath': source, 'pbrt': source})
        self.assertEqual(seeds, {'rypath': list(range(16)), 'pbrt': list(range(16, 32))})
        self.assertEqual(len(statistics_record(result)), 15)

    def test_reference_diagnostic_is_not_a_pass(self):
        def runner(command, **kwargs):
            return subprocess.CompletedProcess(command, 0, '', 'ERROR invalid sample')

        with tempfile.TemporaryDirectory() as tmp:
            variant = Path(tmp)/'invalid.pbrt'
            with self.assertRaisesRegex(ValueError, 'renderer reported ERROR'):
                render_pfm('pbrt', Path('/fake/pbrt'), variant,
                           staged_source('diffuse-direction'), runner=runner)
            record = json.loads(variant.with_suffix('.process.json').read_text())
            self.assertTrue(record['error_diagnostic'])
            self.assertIn('--render-coord-sys', record['command'])

    def test_independent_seed_gate_and_bias_sensitivities(self):
        values = [(1+.001*(i % 3), 2, 3) for i in range(16)]
        data = {'rypath': {'full': values}, 'pbrt': {'full': values}}
        self.assertTrue(all(r['passed'] for r in statistics_record(data)))
        # Missing delta NEE, erroneous MIS downweighting, sign/normalization,
        # or infinite extinction must not become plausible finite black images.
        for factor in (0, .5, 2):
            biased = copy.deepcopy(data)
            biased['rypath']['full'] = [tuple(factor*x for x in rgb) for rgb in values]
            self.assertTrue(all(not r['passed'] for r in statistics_record(biased)))
        # The fixed deterministic numerical allowance is independently bounded.
        low = {'rypath': {'full': [(1e-4,)*3]*16}, 'pbrt': {'full': [(0.,)*3]*16}}
        self.assertTrue(all(not r['passed'] for r in statistics_record(low)))
        for invalid in ([(float('nan'), 2, 3)]*16, [(1, 2)]*16,
                        [(1, 2, 3)]*8, [(1, 2, 3)]*17):
            bad = copy.deepcopy(data)
            bad['rypath']['full'] = invalid
            with self.assertRaises(ValueError):
                statistics_record(bad)


if __name__ == '__main__':
    unittest.main()
