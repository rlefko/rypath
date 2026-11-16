#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fixture closure, seed independence, omission equivalence and retained commands."""
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

import compare_sampling_defaults as suite
import generate_sampling_default_samples as preview


class SamplingDefaultsTests(unittest.TestCase):
    def test_all_variants_preserve_world_and_closure(self):
        self.assertEqual(len(suite.CASES), 18)
        closure = json.loads((suite.FIXTURES/'dependencies.json').read_text())
        manifest = json.loads((suite.FIXTURES/'manifest.json').read_text())
        self.assertEqual(set(manifest), set(suite.SCENES+suite.DEFAULT_SCENES))
        self.assertEqual(set(closure), set(manifest))
        for case in suite.CASES:
            name, source = suite.case_source(case)
            base = suite.staged_source(name)
            self.assertEqual(base.split(b'WorldBegin')[1], source.split(b'WorldBegin')[1])
            self.assertIn(b'PixelFilter "gaussian"', source)
            self.assertEqual(source.count(b'"integer pixelsamples" [4096]'), 1)
            self.assertEqual(source.count(suite.SEED_TOKEN), 1)
            kind, randomization = suite.CONFIGURATIONS[suite.CASES.index(case)][1].split('-')
            self.assertIn(f'Sampler "{kind}" "string randomization" ["{randomization}"]'.encode(), source)
            with tempfile.TemporaryDirectory() as tmp:
                suite.comparison.stage_dependencies(suite.FIXTURES/(name+'.pbrt'), Path(tmp))
                for record in closure[name]:
                    data = (Path(tmp)/record['path']).read_bytes()
                    self.assertEqual(len(data), record['bytes'])
                    self.assertEqual(hashlib.sha256(data).hexdigest(), record['sha256'])
        for name in suite.DEFAULT_SCENES:
            suite.staged_source(name)
        with self.assertRaises(ValueError):
            suite.case_source('../outside')
        with self.assertRaises(ValueError):
            suite.staged_source('../outside')

    def test_filter_sensitivity_and_defaults(self):
        coarse = suite.staged_source('edge-coarse')
        self.assertIn(b'"float xradius" [.0625]', coarse)
        self.assertIn(b'"float yradius" [.0625]', coarse)
        self.assertIn(b'"float sigma" [.01]', coarse)
        self.assertIn(b'[.51 -7 8', coarse)
        self.assertIn(b'PixelFilter "gaussian"\n', suite.staged_source('edge'))
        self.assertIn(b'"float sigma" [0.7]', suite.staged_source('diffuse-area'))

    def test_cornell_attribution_and_input_identity(self):
        provenance = json.loads((suite.FIXTURES/'provenance.json').read_text())
        record, = provenance['external_assets']
        self.assertEqual(record['license'], 'CC0-1.0')
        self.assertEqual(record['upstream_revision'], 'e969701f3ebc0238003150dc0c0e7c3cc93aa1fc')
        upstream = suite.FIXTURES.parent.parent/'pbrt/cornell-box.pbrt'
        self.assertEqual(hashlib.sha256(upstream.read_bytes()).hexdigest(), record['upstream_sha256'])
        self.assertEqual((suite.FIXTURES/record['license_file']).read_bytes(),
                         (upstream.parent/'LICENSE.txt').read_bytes())
        self.assertIn('no new unmodified upstream', provenance['coverage'])

    def test_randomized_cases_have_independent_seed_schedule(self):
        calls = []

        def renderer(backend, binary, variant, source):
            calls.append((backend, source))
            return [(1., 2., 3.)]*64

        with tempfile.TemporaryDirectory() as tmp:
            measurements = suite.render_case('edge-halton-permutedigits', Path('/ours'),
                Path('/reference'), Path(tmp), renderer=renderer)
        self.assertEqual(len(calls), 32)
        for backend, start in (('rypath', 0), ('pbrt', 16)):
            sources = [source for b, source in calls if b == backend]
            self.assertEqual([int(re.search(rb'"integer seed" \[(\d+)\]', s)[1])
                              for s in sources], list(range(start, start+16)))
            self.assertTrue(all(b'Sampler "halton"' in s for s in sources))
            self.assertTrue(all(len(values) == 16 for values in measurements[backend].values()))
        self.assertTrue(all(g['passed'] for g in suite.statistics_record(measurements)))

    def test_no_deterministic_halton_replicate_intervals(self):
        self.assertEqual({s for _, s in suite.CONFIGURATIONS}, set(suite.SAMPLERS))
        self.assertNotIn('halton-none', suite.SAMPLERS)
        self.assertNotIn('halton-owen', suite.SAMPLERS)

    def test_defaults_are_compared_within_backend_without_source_overrides(self):
        calls = []

        def renderer(backend, binary, variant, source):
            calls.append((backend, variant.stem, source))
            return [(1., 2., 3.)]*64

        with tempfile.TemporaryDirectory() as tmp:
            result = suite.check_defaults(Path('/ours'), Path('/reference'), Path(tmp), renderer=renderer)
        self.assertEqual(len(calls), 4)
        self.assertTrue(all(record['passed'] for record in result.values()))
        for _, name, source in calls:
            self.assertEqual(source, suite.staged_source(name))
            self.assertIn(b'Film "rgb"', source)
            if name == 'implicit-defaults':
                self.assertFalse(re.search(rb'^(Camera|Sampler|Integrator|PixelFilter)\s', source, re.M))
            else:
                self.assertIn(b'"integer pixelsamples" [16]', source)
                self.assertIn(b'Sampler "zsobol"', source)
                self.assertIn(b'Integrator "volpath"', source)

    def test_default_disagreement_is_failure(self):
        def renderer(backend, binary, variant, source):
            return [(2. if variant.stem == 'explicit-defaults' else 1., 1., 1.)]*64

        with tempfile.TemporaryDirectory() as tmp:
            result = suite.check_defaults(Path('/ours'), Path('/reference'), Path(tmp), renderer=renderer)
        self.assertFalse(any(record['passed'] for record in result.values()))

    def test_statistical_gate_rejects_bias_and_invalid_replicates(self):
        values = {backend: {region: [[1., 2., 3.]]*16 for region in suite.comparison.QUADRANTS}
                  for backend in ('rypath', 'pbrt')}
        changed = copy.deepcopy(values)
        changed['rypath']['full'] = [[1.1, 2., 3.]]*16
        self.assertFalse(all(g['passed'] for g in suite.statistics_record(changed)))
        values['rypath']['full'].pop()
        with self.assertRaises(ValueError):
            suite.statistics_record(values)

    def test_mocked_runner_retains_reference_command_and_input(self):
        commands = []

        def runner(command, **kwargs):
            commands.append(command.copy())
            output = Path(command[command.index('--outfile')+1]) if '--outfile' in command else Path(command[-1])
            output.write_bytes(b'PF\n8 8\n-1.0\n'+struct.pack('<fff', 1., 2., 3.)*64)
            return subprocess.CompletedProcess(command, 0, stdout='reference transcript', stderr='')

        with tempfile.TemporaryDirectory() as tmp:
            variant = Path(tmp)/'implicit.pbrt'
            source = suite.staged_source('implicit-defaults')
            image = suite.render_pfm('pbrt', Path('/reference'), variant, source, runner=runner)
            self.assertEqual(image, [(1., 2., 3.)]*64)
            self.assertEqual(variant.read_bytes(), source)
            retained = json.loads(variant.with_suffix('.process.json').read_text())
            self.assertEqual(retained['command'], commands[0])
            self.assertEqual(commands[0][1:5], ['--render-coord-sys', 'world', '--nthreads', '1'])
            self.assertNotIn('--spp', commands[0])
            self.assertNotIn('--seed', commands[0])

    def test_preview_generation_and_mock_render(self):
        with tempfile.TemporaryDirectory() as tmp:
            destination = Path(tmp)
            metadata = preview.generate(destination, samples=128, width=192, height=128)
            self.assertEqual(len(metadata['previews']), 3)
            defaults = (destination/'implicit-defaults.pbrt').read_text()
            self.assertFalse(re.search(r'^(Sampler|Camera|Integrator|PixelFilter)\s', defaults, re.M))
            self.assertIn('"integer xresolution" [192]', defaults)
            self.assertIn('"integer pixelsamples" [128]', (destination/'halton-studio.pbrt').read_text())
            self.assertIn('Sampler "zsobol"', (destination/'zsobol-cornell.pbrt').read_text())
            self.assertIn('16 samples per pixel', (destination/'index.html').read_text())

            def runner(command, **kwargs):
                Path(command[command.index('--output')+1]).write_bytes(b'fake PNG for mocked execution')
                return subprocess.CompletedProcess(command, 0, stdout='complete', stderr='')

            preview.render(destination, Path('/rypath'), metadata, runner=runner)
            actual = json.loads((destination/'previews.json').read_text())
            self.assertTrue(all(record['image_sha256'] for record in actual['previews']))


if __name__ == '__main__':
    unittest.main()
