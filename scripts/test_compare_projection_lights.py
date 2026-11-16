#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Projection reference inputs, execution provenance, and independent seed gates."""
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

from compare_projection_lights import (FIXTURES, SCENES, REVISION, backend_sources,
                                    render_pfm, render_backend_sources, staged_source, statistics_record)
from compare_pbrt import stage_dependencies
from generate_projection_samples import generate, pattern_png


class ProjectionComparisonTests(unittest.TestCase):
    def test_owned_fixture_protocol_and_dependency_closure(self):
        self.assertEqual(len(SCENES), 11)
        provenance = json.loads((FIXTURES/'provenance.json').read_text())
        self.assertEqual(provenance['pbrt_revision'], REVISION)
        self.assertEqual(provenance['reference_coordinate_system'], 'world')
        self.assertEqual(provenance['license'], 'GPL-3.0-only')
        self.assertEqual(len(provenance['external_assets']), 1)
        self.assertEqual(set(provenance['scenes']), set(SCENES))
        closure = json.loads((FIXTURES/'dependencies.json').read_text())
        for name in SCENES:
            source = staged_source(name)
            for token in (b'LightSource "projection"', b'"integer pixelsamples" [4096]',
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

    def test_external_map_terms_and_unchanged_asset(self):
        record = json.loads((FIXTURES/'provenance.json').read_text())['external_assets'][0]
        self.assertEqual(record['author'], 'Willem van Schaik')
        self.assertEqual(record['upstream_revision'], 'af1a5bc352164740c1cc1354942b1c6b72eacb8a')
        self.assertTrue(record['unchanged'])
        self.assertEqual((FIXTURES/record['path']).read_bytes(),
                         (FIXTURES.parent/'image-assets/z03n2c08.png').read_bytes())
        self.assertEqual((FIXTURES/record['license_file']).read_bytes(),
                         (FIXTURES.parent/'image-assets/PngSuite.LICENSE').read_bytes())

    def test_semantic_coverage(self):
        constant = staged_source('constant')
        self.assertNotIn(b'"float scale"', constant)
        self.assertNotIn(b'"float power"', constant)
        self.assertNotIn(b'"float fov"', constant.split(b'WorldBegin')[1])
        for name in SCENES:
            light = next(line for line in staged_source(name).splitlines()
                         if b'LightSource "projection"' in line)
            self.assertNotIn(b'"rgb I"', light)
            self.assertNotIn(b'"spectrum I"', light)
            self.assertNotIn(b'"rgb L"', light)
        for name, dimensions in [('landscape', (12, 6)), ('portrait', (6, 12))]:
            data = (FIXTURES/'assets'/(name+'.png')).read_bytes()
            self.assertEqual(struct.unpack('>II', data[16:24]), dimensions)
            self.assertEqual(data[25], 2)  # RGB, not grayscale scalar data
        self.assertIn(b'ConcatTransform', staged_source('affine'))
        self.assertIn(b'"float power" [1200]', staged_source('power'))
        self.assertIn(b'"float scale" [.7]', staged_source('power'))
        for kind in ('point', 'spot', 'distant', 'infinite'):
            self.assertIn(f'LightSource "{kind}"'.encode(), staged_source('mixed-lighting'))
        self.assertIn(b'Material "conductor"', staged_source('rough-reflection'))
        self.assertIn(b'Material "dielectric"', staged_source('rough-transmission'))
        self.assertIn(b'Material "interface"', staged_source('bounded-volume'))
        empty = staged_source('geometry-free-volume')
        self.assertLess(empty.index(b'MediumInterface "" "fog"'), empty.index(b'Camera'))
        self.assertNotIn(b'Shape ', empty)
        for name in ('bounded-volume', 'geometry-free-volume'):
            self.assertIn(b'Integrator "volpath"', staged_source(name))

    def test_sample_generator_retains_chart_and_render_contract(self):
        chart = pattern_png()
        self.assertEqual(chart, pattern_png())
        self.assertEqual(struct.unpack('>II', chart[16:24]), (384, 240))
        self.assertEqual(chart[25], 2)
        with tempfile.TemporaryDirectory() as tmp:
            destination = Path(tmp)
            for samples in (512, 4096):
                generate(destination, samples)
                self.assertEqual((destination/'projection-chart.png').read_bytes(), chart)
                for name, integrator in [('projection-studio', 'path'), ('projection-fog', 'volpath')]:
                    scene = (destination/(name+'.pbrt')).read_text()
                    for token in ('"integer xresolution" [768]', '"integer yresolution" [512]',
                                  f'"integer pixelsamples" [{samples}]', 'PixelFilter "triangle"',
                                  f'Integrator "{integrator}"', 'LightSource "projection"',
                                  'ConcatTransform', '"float power"', 'projection-chart.png'):
                        self.assertIn(token, scene)
                    self.assertNotIn('Shape "sphere"', scene)
                    self.assertIn('Shape "trianglemesh"', scene)
                fog = (destination/'projection-fog.pbrt').read_text()
                self.assertIn('Material "interface"', fog)
                self.assertIn('MediumInterface "haze" ""', fog)

    def test_backend_sources_and_explicit_world_override(self):
        for name in SCENES:
            source = staged_source(name)
            sources, overrides = backend_sources(name, source)
            self.assertEqual(sources, {'rypath': source, 'pbrt': source})
            self.assertEqual(overrides['rypath'], [])
            self.assertEqual(overrides['pbrt'][0]['parameter'], '--render-coord-sys')
            self.assertEqual(overrides['pbrt'][0]['comparison'], 'world')
            self.assertIn('finite source position', overrides['pbrt'][0]['reason'])

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
                                    staged_source('constant'), runner=runner)
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

        source = staged_source('constant')
        with tempfile.TemporaryDirectory() as tmp:
            with patch('compare_projection_lights.render_pfm', side_effect=render):
                result = render_backend_sources(Path('/fake/ours'), Path('/fake/pbrt'),
                    FIXTURES/'constant.pbrt', Path(tmp),
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
                           staged_source('constant'), runner=runner)
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
