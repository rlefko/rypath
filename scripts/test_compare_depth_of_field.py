#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Depth-of-field fixture closure, fixed statistical protocol and scene-only samples."""
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import compare_depth_of_field as suite
from compare_pbrt import stage_dependencies
from generate_depth_of_field_samples import generate


class DepthOfFieldComparisonTests(unittest.TestCase):
    def test_fixture_hashes_closure_and_provenance(self):
        provenance = json.loads((suite.FIXTURES/'provenance.json').read_text())
        closure = json.loads((suite.FIXTURES/'dependencies.json').read_text())
        self.assertEqual(provenance['pbrt_revision'], suite.REVISION)
        self.assertEqual(set(provenance['scenes']), set(suite.SCENES))
        for name in suite.SCENES:
            source = suite.staged_source(name)
            for token in (b'"integer pixelsamples" [4096]', b'"integer seed" [0]',
                          b'"integer xresolution" [8]', b'"integer yresolution" [8]',
                          b'"string sensor" "cie1931"', b'Camera "perspective"'):
                self.assertIn(token, source)
            paths = provenance['scenes'][name]['dependencies']
            self.assertEqual(paths, [r['path'] for r in closure[name]])
            filenames = re.findall(rb'"string filename" \["([^"\n]+)"\]', source)
            self.assertTrue(set(p.decode() for p in filenames).issubset(paths))
            with tempfile.TemporaryDirectory() as tmp:
                stage_dependencies(suite.FIXTURES/(name+'.pbrt'), Path(tmp))
                for record in closure[name]:
                    payload = (Path(tmp)/record['path']).read_bytes()
                    self.assertEqual(len(payload), record['bytes'])
                    self.assertEqual(hashlib.sha256(payload).hexdigest(), record['sha256'])
        for record in provenance['owned_assets']:
            self.assertEqual(hashlib.sha256((suite.FIXTURES/record['path']).read_bytes()).hexdigest(), record['sha256'])
        external = provenance['external_assets'][0]
        self.assertEqual(external['license'], 'CC0-1.0')
        self.assertEqual((suite.FIXTURES/external['license_file']).read_bytes(),
                         (suite.FIXTURES.parent.parent/'pbrt/LICENSE.txt').read_bytes())
        for location, field in [('pbrt/cornell-box.pbrt', 'original'),
                                ('pbrt-render/cornell-box-adapted.pbrt', 'adapted_source')]:
            data = (suite.FIXTURES.parent.parent/location).read_bytes()
            self.assertEqual(len(data), external[field]['bytes'])
            self.assertEqual(hashlib.sha256(data).hexdigest(), external[field]['sha256'])
        with self.assertRaises(ValueError):
            suite.staged_source('../outside')

    def test_variants_preserve_camera_and_scene(self):
        self.assertEqual(len(suite.CASES), 24)
        for case in suite.CASES:
            name, source = suite.case_source(case)
            base = suite.staged_source(name)
            self.assertEqual(base.split(b'Camera ')[1].split(b'Film ')[0],
                             source.split(b'Camera ')[1].split(b'Film ')[0])
            self.assertEqual(base.split(b'WorldBegin')[1], source.split(b'WorldBegin')[1])
            triangle = '-triangle-' in case
            self.assertIn(b'PixelFilter "triangle"' if triangle else b'PixelFilter "box"', source)
            for axis in ('x', 'y'):
                self.assertIn(f'"float {axis}radius" [{1 if triangle else "0.5"}]'.encode(), source)
            if case.endswith('sobol-fastowen'):
                self.assertIn(b'Sampler "sobol" "string randomization" ["fastowen"]', source)
            else:
                self.assertIn(b'Sampler "independent"', source)
        self.assertNotIn(b'focaldistance', suite.staged_source('default-focus'))
        self.assertIn(b'"float lensradius" [0]', suite.staged_source('pinhole-zero'))
        self.assertIn(b'"string filter" ["ewa"]', suite.staged_source('texture-filtered'))
        with self.assertRaises(ValueError):
            suite.case_source('invalid')

    def test_fixed_seeds_and_case_directories(self):
        seeds = {'rypath': [], 'pbrt': []}
        def render(backend, binary, variant, source):
            seeds[backend].append(int(re.search(rb'"integer seed" \[(\d+)\]', source)[1]))
            self.assertEqual(variant.parent.parent.name, 'focus-plane-triangle-sobol-fastowen')
            return [(1., 2., 3.)]*64
        name, source = suite.case_source('focus-plane-triangle-sobol-fastowen')
        with tempfile.TemporaryDirectory() as tmp, patch.object(suite, 'render_pfm', side_effect=render):
            result = suite.render_backend_sources(Path('/fake/ours'), Path('/fake/pbrt'),
                suite.FIXTURES/(name+'.pbrt'), Path(tmp), {'rypath': source, 'pbrt': source},
                case='focus-plane-triangle-sobol-fastowen')
        self.assertEqual(seeds, {'rypath': list(range(16)), 'pbrt': list(range(16, 32))})
        self.assertEqual(len(suite.statistics_record(result)), 15)

    def test_retained_process_and_diagnostics(self):
        def runner(command, **kwargs):
            output = Path(command[command.index('--outfile')+1])
            output.write_bytes(b'PF\n8 8\n-1\n'+struct.pack('<192f', *([1.]*192)))
            return subprocess.CompletedProcess(command, 0, 'retained output', '')
        with tempfile.TemporaryDirectory() as tmp:
            variant = Path(tmp)/'case.pbrt'
            suite.render_pfm('pbrt', Path('/fake/pbrt'), variant, suite.staged_source('focus-plane'), runner=runner)
            record = json.loads(variant.with_suffix('.process.json').read_text())
            self.assertEqual(record['command'][1:5], ['--render-coord-sys', 'world', '--nthreads', '2'])
            self.assertEqual(variant.with_suffix('.stdout.log').read_text(), 'retained output')
            with self.assertRaisesRegex(ValueError, 'renderer reported ERROR'):
                suite.render_pfm('pbrt', Path('/fake/pbrt'), variant, b'',
                    runner=lambda cmd, **kwargs: subprocess.CompletedProcess(cmd, 0, '', 'ERROR invalid'))

    def test_predeclared_gate_detects_bias_and_rejects_invalid_means(self):
        data = {'rypath': {'full': [(1., 2., 3.)]*16}, 'pbrt': {'full': [(1., 2., 3.)]*16}}
        self.assertTrue(all(r['passed'] for r in suite.statistics_record(data)))
        for factor in (.5, 2):
            biased = copy.deepcopy(data)
            biased['rypath']['full'] = [(factor, 2*factor, 3*factor)]*16
            self.assertTrue(all(not r['passed'] for r in suite.statistics_record(biased)))
        for values in ([(float('nan'), 2, 3)]*16, [(1, 2)]*16, [(1, 2, 3)]*15):
            invalid = copy.deepcopy(data)
            invalid['rypath']['full'] = values
            with self.assertRaises(ValueError):
                suite.statistics_record(invalid)

    def test_black_backend_is_not_a_valid_comparison(self):
        for black_backends in (('rypath',), ('pbrt',), ('rypath', 'pbrt')):
            data = {'rypath': {'full': [(1., 2., 3.)]*16},
                    'pbrt': {'full': [(1., 2., 3.)]*16}}
            for backend in black_backends:
                data[backend]['full'] = [(0., 0., 0.)]*16
            with self.assertRaisesRegex(ValueError, 'no positive signal'):
                suite.statistics_record(data)

    def test_showcase_matched_except_camera(self):
        with tempfile.TemporaryDirectory() as tmp:
            destination = Path(tmp)
            generate(destination)
            scenes = [(destination/(name+'.pbrt')).read_text()
                      for name in ('pinhole', 'focus-near', 'focus-far')]
            bodies = [s.split('WorldBegin')[1] for s in scenes]
            self.assertEqual(bodies, [bodies[0]]*3)
            for source in scenes:
                for token in ('"integer xresolution" [768]', '"integer yresolution" [512]',
                              '"integer pixelsamples" [512]', 'PixelFilter "triangle"',
                              '"string filter" ["ewa"]', '"texture reflectance" "chart"'):
                    self.assertIn(token, source)
                self.assertNotIn('AreaLightSource', source)
            self.assertIn('"float lensradius" [0]', scenes[0])
            self.assertIn('"float focaldistance" [4]', scenes[1])
            self.assertIn('"float focaldistance" [10]', scenes[2])
            chart = (destination/'focus-chart.png').read_bytes()
            generate(destination)
            self.assertEqual(chart, (destination/'focus-chart.png').read_bytes())


if __name__ == '__main__':
    unittest.main()
