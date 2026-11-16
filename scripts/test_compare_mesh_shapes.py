#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fixture closure, retained evidence, geometry oracle, and statistical failures."""
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import compare_mesh_shapes as suite
from compare_loop_subdivision import compare_geometry, control_meshes, probe_input
from compare_pbrt import stage_dependencies
from generate_mesh_shape_samples import generate


class MeshShapeTests(unittest.TestCase):
    def test_closure_and_external_provenance(self):
        provenance = json.loads((suite.FIXTURES/'provenance.json').read_text())
        closure = json.loads((suite.FIXTURES/'dependencies.json').read_text())
        self.assertEqual(provenance['pbrt_revision'], suite.REVISION)
        self.assertEqual(set(provenance['scenes']), set(suite.SCENES))
        for name in suite.SCENES:
            source = suite.staged_source(name)
            for token in (b'"integer pixelsamples" [4096]', b'"integer seed" [0]',
                          b'"integer xresolution" [8]', b'"integer yresolution" [8]',
                          b'"string sensor" "cie1931"'):
                self.assertIn(token, source)
            paths = [r['path'] for r in closure[name]]
            self.assertEqual(provenance['scenes'][name]['dependencies'], paths)
            includes = re.findall(rb'Include "([^"]+)"', source)
            self.assertTrue(set(p.decode() for p in includes).issubset(paths))
            filenames = re.findall(rb'"string filename" \["([^"]+)"\]', source)
            self.assertTrue(set(p.decode() for p in filenames).issubset(paths))
            with tempfile.TemporaryDirectory() as tmp:
                stage_dependencies(suite.FIXTURES/(name+'.pbrt'), Path(tmp))
                for record in closure[name]:
                    data = (Path(tmp)/record['path']).read_bytes()
                    self.assertEqual(len(data), record['bytes'])
                    self.assertEqual(hashlib.sha256(data).hexdigest(), record['sha256'])
        self.assertIn('selected-asset', provenance['coverage'])
        self.assertEqual([(r['control_faces'], r['level2_faces'])
                          for r in provenance['external_assets']],
                         [(2048, 32768), (2336, 37376)])
        for record in provenance['external_assets']:
            self.assertEqual(record['license'], 'CC0')
            self.assertEqual(record['upstream_revision'],
                             '30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572')
            self.assertTrue(record['unchanged'])
            data = (suite.FIXTURES/record['path']).read_bytes()
            self.assertEqual(len(data), record['bytes'])
            self.assertEqual(hashlib.sha256(data).hexdigest(), record['sha256'])
            self.assertEqual(len(record['source_sha256']), 64)
            license_text = (suite.FIXTURES/record['license_file']).read_text()
            self.assertIn('Mareck', license_text)
            self.assertIn('Public Domain', license_text)
        for record in provenance['owned_assets']:
            data = (suite.FIXTURES/record['path']).read_bytes()
            self.assertEqual(len(data), record['bytes'])
            self.assertEqual(hashlib.sha256(data).hexdigest(), record['sha256'])
            self.assertEqual(record['license'], 'GPL-3.0-only')

    def test_sampler_variants_and_world_override(self):
        self.assertEqual(len(suite.CASES), 24)
        for case in suite.CASES:
            name, source = suite.case_source(case)
            self.assertEqual(source.split(b'WorldBegin')[1],
                             suite.staged_source(name).split(b'WorldBegin')[1])
            backends, overrides = suite.backend_sources(name, source)
            self.assertEqual(backends['pbrt'], backends['rypath'])
            self.assertEqual(overrides['pbrt'][0]['comparison'], 'world')
            self.assertEqual(overrides['rypath'], [])
        for call in (suite.case_source, suite.staged_source):
            with self.assertRaises(ValueError):
                call('../outside')

    def test_seed_schedule(self):
        seeds = {'rypath': [], 'pbrt': []}

        def render(backend, binary, variant, source):
            seeds[backend].append(int(re.search(rb'"integer seed" \[(\d+)\]', source)[1]))
            return [(1., 2., 3.)] * 64

        name, source = suite.case_source('quad-textured-sobol-fastowen')
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(suite, 'render_pfm', side_effect=render):
            result = suite.render_backend_sources(
                Path('/fake/ours'), Path('/fake/pbrt'),
                suite.FIXTURES/(name+'.pbrt'), Path(tmp),
                {'rypath': source, 'pbrt': source})
        self.assertEqual(seeds, {'rypath': list(range(16)), 'pbrt': list(range(16, 32))})
        self.assertEqual(len(suite.statistics_record(result)), 15)

    def test_reported_errors_are_not_success(self):
        def runner(command, **kwargs):
            output = Path(command[command.index('--outfile')+1])
            output.write_bytes(b'PF\n8 8\n-1\n'+struct.pack('<192f', *([1.]*192)))
            return subprocess.CompletedProcess(command, 0, 'retained output', '')

        with tempfile.TemporaryDirectory() as tmp:
            variant = Path(tmp)/'case.pbrt'
            suite.render_pfm('pbrt', Path('/fake/pbrt'), variant,
                             suite.staged_source('quad-textured'), runner=runner)
            process = json.loads(variant.with_suffix('.process.json').read_text())
            self.assertEqual(process['command'][1:5],
                             ['--render-coord-sys', 'world', '--nthreads', '1'])
            self.assertEqual(variant.with_suffix('.stdout.log').read_text(),
                             'retained output')
            for code, diagnostic in ((0, 'ERROR invalid'), (1, 'failed')):
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    suite.render_pfm(
                        'pbrt', Path('/fake/pbrt'), variant, b'',
                        runner=lambda cmd, **kw: subprocess.CompletedProcess(
                            cmd, code, '', diagnostic))

    def test_gate_and_invalid_seed_means(self):
        data = {'rypath': {'full': [(1., 2., 3.)]*16},
                'pbrt': {'full': [(1., 2., 3.)]*16}}
        self.assertTrue(all(r['passed'] for r in suite.statistics_record(data)))
        bias = copy.deepcopy(data)
        bias['rypath']['full'] = [(2., 4., 6.)]*16
        self.assertTrue(all(not r['passed'] for r in suite.statistics_record(bias)))
        for values in ([(float('nan'), 2, 3)]*16, [(1, 2)]*16,
                       [(1, 2, 3)]*15, [(0, 0, 0)]*16):
            bad = copy.deepcopy(data)
            bad['rypath']['full'] = values
            with self.assertRaises(ValueError):
                suite.statistics_record(bad)

    def test_tampering_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = Path(tmp)/'fixtures'
            shutil.copytree(suite.FIXTURES, fixture)
            with patch.object(suite, 'FIXTURES', fixture):
                (fixture/'quad-powered.pbrt').write_bytes(b'corrupt')
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    suite.staged_source('quad-powered')
            (fixture/'assets/bathroom-6040.pbrt').write_bytes(b'corrupt')
            with self.assertRaises(ValueError):
                stage_dependencies(fixture/'bathroom-6040.pbrt', Path(tmp)/'stage')

    def test_reference_revision_and_cleanliness(self):
        binary = Path('/cache/build/pbrt')
        with patch.object(suite.subprocess, 'check_output',
                          side_effect=[suite.REVISION+'\n', '']):
            self.assertEqual(suite.verify_reference(binary), suite.REVISION)
        with patch.object(suite.subprocess, 'check_output', return_value='wrong'):
            with self.assertRaises(ValueError):
                suite.verify_reference(binary)
        with patch.object(suite.subprocess, 'check_output',
                          side_effect=[suite.REVISION, ' M source']):
            with self.assertRaises(ValueError):
                suite.verify_reference(binary)

    def test_geometry_oracle_failures(self):
        data = {'indices': [0, 1, 2], 'P': [[0, 0, 0], [1, 0, 0], [0, 1, 0]],
                'N': [[0, 0, 1]]*3}
        self.assertEqual(compare_geometry(data, data)['P']['max_scaled_error'], 0)
        mutations = (('indices', [0, 2, 1]),
                     ('P', [[.1, 0, 0], [1, 0, 0], [0, 1, 0]]),
                     ('N', [[0, 0, float('nan')]]*3), ('N', [[0, 0, 2]]*3))
        for field, value in mutations:
            bad = copy.deepcopy(data)
            bad[field] = value
            with self.assertRaises(ValueError):
                compare_geometry(bad, data)
        tiny = copy.deepcopy(data)
        tiny['N'] = [[1e-10, 2e-10, 3e-10]]*3
        for scale in (0, .5, 2):
            bad = copy.deepcopy(tiny)
            bad['N'][1] = [x*scale for x in tiny['N'][1]]
            with self.assertRaises(ValueError):
                compare_geometry(bad, tiny)
        meshes = control_meshes()
        self.assertEqual(len(meshes['bathroom-6040'][1])//3, 2048)
        self.assertEqual(len(meshes['bathroom-6995'][1])//3, 2336)
        self.assertTrue(probe_input(*meshes['tetra'], 2).startswith('2 4 12\n'))

    def test_cli_rejects_unapproved_replicate_count(self):
        run = subprocess.run(
            ['python3', str(suite.ROOT/'scripts/compare_mesh_shapes.py'),
             '--output-dir', '/unused', '--pbrt-only', '--replicates', '2'],
            capture_output=True, text=True)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('requires exactly sixteen', run.stderr)

    def test_failed_render_retains_incomplete_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = root/'pbrt'
            binary.write_bytes(b'fake reference')
            argv = ['compare_mesh_shapes.py', '--pbrt', str(binary),
                    '--output-dir', str(root/'output'), '--pbrt-only',
                    '--case', 'quad-textured-independent']
            with patch('sys.argv', argv), \
                    patch.object(suite, 'verify_reference', return_value=suite.REVISION), \
                    patch.object(suite.subprocess, 'check_output', return_value='revision'), \
                    patch.object(suite, 'render_pfm', side_effect=ValueError('render failed')):
                with self.assertRaisesRegex(ValueError, 'render failed'):
                    suite.main()
            work = next((root/'output').iterdir())
            summary = json.loads((work/'summary.json').read_text())
            self.assertFalse(summary['passed'])
            self.assertFalse(summary['complete'])
            self.assertEqual(summary['scenes']['quad-textured-independent']['error'],
                             'render failed')
            provenance = json.loads((work/'provenance.json').read_text())
            self.assertEqual(provenance['reference_execution']['threads'], 1)
            self.assertIn('quad-textured-independent', provenance['fixtures'])

    def test_original_sample_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = generate(tmp)
            self.assertEqual(len(paths), 5)
            for path in paths:
                source = path.read_text()
                for token in ('"integer xresolution" [768]',
                              '"integer yresolution" [512]',
                              '"integer pixelsamples" [512]'):
                    self.assertIn(token, source)
            self.assertIn('Shape "trianglemesh"', (Path(tmp)/'control.pbrt').read_text())
            for level in (0, 1, 3):
                self.assertIn(f'"integer levels" [{level}]',
                              (Path(tmp)/f'loop-{level}.pbrt').read_text())
            self.assertIn('"texture reflectance" "chart"',
                          (Path(tmp)/'curved-quad.pbrt').read_text())


if __name__ == '__main__':
    unittest.main()
