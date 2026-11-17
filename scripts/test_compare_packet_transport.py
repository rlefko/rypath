#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline protocol, fixture closure, process diagnostics and fixed-gate checks."""
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

import compare_packet_transport as suite


def measurements():
    return {region: [[1., 2., 3.]] * 16 for region in suite.comparison.QUADRANTS}


class PacketTransportTests(unittest.TestCase):
    def test_verified_cases_and_world_preservation(self):
        self.assertEqual(suite.DEFAULT_CASES, (
            'cornell', 'glass-smooth', 'glass-rough', 'glass-delta-chain', 'mirror',
            'constant-environment', 'image-environment', 'mixed-delta-lights',
            'lens-gaussian', 'visible-clamp'))
        self.assertIn('coated', suite.CASES)
        self.assertNotIn('coated', suite.DEFAULT_CASES)
        for case in suite.CASES:
            original = suite.staged_source(case)
            variant, overrides = suite.case_source(case)
            self.assertEqual(original.split(b'WorldBegin', 1)[1],
                             variant.split(b'WorldBegin', 1)[1])
            configured = suite.CONFIG['cases'][case].get('integrator', 'bdpt')
            self.assertIn(f'Integrator "{configured}"'.encode(), variant)
            self.assertEqual(variant.count(suite.SEED_TOKEN), 1)
            self.assertEqual(variant.count(b'"integer pixelsamples" [4096]'), 1)
            if case in ('coated-path', 'coated-samples16'):
                self.assertEqual(variant, original)
                self.assertEqual(overrides, [])
                self.assertNotIn(case, suite.DEFAULT_CASES)
            else:
                self.assertIn('Integrator', [r['directive'] for r in overrides])
            with tempfile.TemporaryDirectory() as tmp:
                suite.stage_dependencies(case, Path(tmp))
                for record in suite.CONFIG['cases'][case]['dependencies']:
                    data = (Path(tmp) / record['path']).read_bytes()
                    self.assertEqual(len(data), record['bytes'])
                    self.assertEqual(hashlib.sha256(data).hexdigest(), record['sha256'])
        with self.assertRaises(ValueError):
            suite.case_source('../outside')
        with self.assertRaises(ValueError):
            suite.replace_descriptor(b'WorldBegin\n', 'Integrator', b'Integrator "bdpt"')

    def test_integrator_defaults_selection_and_exact_override(self):
        record = suite.CONFIG['cases']['coated-path']
        path_source, _ = suite.case_source('coated-path')
        self.assertIn(b'Integrator "path" "integer maxdepth" [5]', path_source)
        self.assertIn(b'"float maxcomponentvalue" [1e30]', path_source)
        with mock.patch.dict(record, {'integrator': 'bdpt'}):
            bdpt_source, overrides = suite.case_source('coated-path')
            self.assertEqual(bdpt_source, path_source.replace(b'Integrator "path"',
                                                            b'Integrator "bdpt"', 1))
            self.assertEqual(overrides, [{
                'directive': 'Integrator',
                'authored': 'Integrator "path" "integer maxdepth" [5]',
                'comparison': 'Integrator "bdpt" "integer maxdepth" [5]'}])
        default_record = copy.deepcopy(record)
        default_record.pop('integrator')
        with mock.patch.dict(suite.CONFIG['cases'], {'coated-path': default_record}):
            self.assertEqual(suite.case_source('coated-path')[0], bdpt_source)
        for integrator in ('simplepath', 'volpath', 'BDPT', '', None):
            with self.subTest(integrator=integrator), mock.patch.dict(
                    record, {'integrator': integrator}):
                with self.assertRaisesRegex(ValueError, 'unsupported comparison integrator'):
                    suite.case_source('coated-path')

    def test_source_hash_and_size_are_verified_for_optional_diagnostics(self):
        for case in ('coated-path', 'coated-samples16'):
            record = suite.CONFIG['cases'][case]
            source = suite.staged_source(case)
            self.assertTrue(source.startswith(b'# SPDX-License-Identifier: GPL-3.0-only'))
            for incorrect in ({'bytes': len(source) + 1}, {'sha256': '0' * 64}):
                with self.subTest(case=case, incorrect=incorrect), mock.patch.dict(
                        record, incorrect):
                    with self.assertRaisesRegex(ValueError, 'source size or checksum mismatch'):
                        suite.case_source(case)
        samples16, _ = suite.case_source('coated-samples16')
        self.assertIn(b'Integrator "bdpt"', samples16)
        self.assertIn(b'"integer nsamples" [16]', samples16)
        original, _ = suite.case_source('coated')
        self.assertIn(b'"integer nsamples" [1]', original)

    def test_clamp_and_connection_expectations_are_separate(self):
        for case in suite.DEFAULT_CASES:
            source, _ = suite.case_source(case)
            if case == 'visible-clamp':
                self.assertIn(b'"integer maxdepth" [0]', source)
                self.assertIn(b'"float maxcomponentvalue" [.002]', source)
                self.assertIn(b'"float scale" [10000]', source)
                self.assertIn(b'"float lensradius" [.05]', source)
                self.assertIn(b'PixelFilter "gaussian"', source)
            else:
                self.assertNotIn(b'"float maxcomponentvalue"', source)
        cornell, _ = suite.case_source('cornell')
        self.assertIn(b'"integer maxdepth" [5]', cornell)
        lens, _ = suite.case_source('lens-gaussian')
        self.assertIn(b'PixelFilter "gaussian"', lens)
        self.assertIn(b'"float lensradius"', lens)
        chain, _ = suite.case_source('glass-delta-chain')
        self.assertIn(b'"integer maxdepth" [6]', chain)
        self.assertEqual(chain.count(b'Shape "trianglemesh"'), 5)

    def test_seed_means_are_independent_replicates(self):
        calls = []

        def renderer(backend, binary, variant, source):
            calls.append((backend, source))
            return [(1., 2., 3.)] * 64

        with tempfile.TemporaryDirectory() as tmp:
            result = {backend: suite.render_backend('cornell', backend, Path('/binary'),
                      Path(tmp), renderer=renderer) for backend in ('rypath', 'pbrt')}
        self.assertEqual(len(calls), 32)
        for backend in ('rypath', 'pbrt'):
            seeds = [int(re.search(rb'"integer seed" \[(\d+)\]', source)[1])
                     for b, source in calls if b == backend]
            self.assertEqual(seeds, suite.SEEDS[backend])
            self.assertEqual(set(result[backend]), set(suite.comparison.QUADRANTS))
            self.assertTrue(all(len(values) == 16 for values in result[backend].values()))
        self.assertTrue(all(r['passed'] for r in suite.statistics_record(result)))
        result['rypath']['full'] = [[1.1, 2., 3.]] * 16
        self.assertFalse(all(r['passed'] for r in suite.statistics_record(result)))
        result['pbrt']['full'].pop()
        with self.assertRaises(ValueError):
            suite.statistics_record(result)

    def test_reference_command_timeout_and_retained_input(self):
        calls = []

        def runner(command, **kwargs):
            calls.append((command.copy(), kwargs))
            output = Path(command[command.index('--outfile') + 1])
            output.write_bytes(b'PF\n8 8\n-1.0\n' + struct.pack('<fff', 1., 2., 3.) * 64)
            return subprocess.CompletedProcess(command, 0, 'reference log', '')

        with tempfile.TemporaryDirectory() as tmp:
            variant = Path(tmp) / '00.pbrt'
            source, _ = suite.case_source('cornell')
            pixels = suite.render_pfm('pbrt', Path('/reference'), variant, source, runner=runner)
            self.assertEqual(pixels, [(1., 2., 3.)] * 64)
            self.assertEqual(variant.read_bytes(), source)
            record = json.loads(variant.with_suffix('.process.json').read_text())
            self.assertEqual(record['command'], calls[0][0])
            self.assertIn('reference log', variant.with_suffix('.stdout.log').read_text())
        self.assertEqual(calls[0][1]['timeout'], 600)
        self.assertEqual(calls[0][0][1:5], ['--render-coord-sys', 'world', '--nthreads', '1'])

    def test_zero_exit_error_and_timeout_are_not_passes(self):
        for failure in ('diagnostic', 'exit', 'timeout', 'launch'):
            def runner(command, **kwargs):
                if failure == 'timeout':
                    raise subprocess.TimeoutExpired(command, 600, output=b'compiling')
                if failure == 'launch':
                    raise OSError('cannot launch renderer')
                return subprocess.CompletedProcess(command, 1 if failure == 'exit' else 0,
                                                   '', 'ERROR: invalid radiance')
            with tempfile.TemporaryDirectory() as tmp:
                variant = Path(tmp) / '00.pbrt'
                with self.assertRaises((ValueError, subprocess.SubprocessError, OSError)):
                    suite.render_pfm('pbrt', Path('/reference'), variant,
                                     suite.case_source('cornell')[0], runner=runner)
                record = json.loads(variant.with_suffix('.process.json').read_text())
                if failure == 'timeout':
                    self.assertTrue(record['timed_out'])
                    self.assertIn('compiling', variant.with_suffix('.stdout.log').read_text())
                elif failure == 'launch':
                    self.assertIn('cannot launch', record['launch_error'])
                else:
                    self.assertTrue(record['error_diagnostic'])

    def test_binary_or_metallib_changes_invalidate_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            artifact = Path(tmp) / 'rypath.metallib'
            artifact.write_bytes(b'compiled pipeline')
            record = {'binaries': {str(artifact): suite.digest(artifact.read_bytes())}}
            suite.verify_artifacts(record)
            artifact.write_bytes(b'changed pipeline')
            with self.assertRaisesRegex(ValueError, 'artifact changed'):
                suite.verify_artifacts(record)

    def test_reference_reuse_checks_inputs_binary_and_complete_regions(self):
        current = {'pbrt_revision': suite.REVISION, 'pbrt_binary_sha256': 'abc',
                   'protocol': suite.CONFIG['protocol'],
                   'cases': {'cornell': {'variant_sha256': 'def', 'dependencies': [],
                                        'reference_cli_overrides': ['--nthreads', '1']}}}
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            suite.write_json(run / 'provenance.json', current)
            suite.write_json(run / 'cornell-measurements.json', {'pbrt': measurements()})
            self.assertEqual(suite.reference_measurements(run, current, 'cornell'), measurements())
            changed = copy.deepcopy(current)
            changed['cases']['cornell']['variant_sha256'] = 'wrong'
            with self.assertRaises(ValueError):
                suite.reference_measurements(run, changed, 'cornell')
            changed = copy.deepcopy(current)
            changed['pbrt_binary_sha256'] = 'wrong'
            with self.assertRaises(ValueError):
                suite.reference_measurements(run, changed, 'cornell')
            partial = measurements()
            partial.pop('full')
            suite.write_json(run / 'cornell-measurements.json', {'pbrt': partial})
            with self.assertRaises(ValueError):
                suite.reference_measurements(run, current, 'cornell')

    def test_reference_reuse_checks_asset_closure_and_cli(self):
        case = 'image-environment'
        current = {'pbrt_revision': suite.REVISION, 'pbrt_binary_sha256': 'abc',
                   'protocol': suite.CONFIG['protocol'], 'manifest_sha256': 'old',
                   'cases': {case: {
                       'variant_sha256': 'unchanged-scene',
                       'dependencies': [{'path': 'bright-float.exr', 'bytes': 123, 'sha256': 'abc'}],
                       'reference_cli_overrides': ['--render-coord-sys', 'world', '--nthreads', '1']}}}
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            suite.write_json(run / 'provenance.json', current)
            suite.write_json(run / f'{case}-measurements.json', {'pbrt': measurements()})
            unrelated = copy.deepcopy(current)
            unrelated['manifest_sha256'] = 'new'
            unrelated['cases']['another-case'] = {}
            self.assertEqual(suite.reference_measurements(run, unrelated, case), measurements())
            for field, value in (('path', 'another.exr'), ('bytes', 124), ('sha256', 'changed')):
                changed = copy.deepcopy(current)
                changed['cases'][case]['dependencies'][0][field] = value
                with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'input mismatch'):
                    suite.reference_measurements(run, changed, case)
            for field, value in (('dependencies', []), ('reference_cli_overrides', ['--nthreads', '2'])):
                changed = copy.deepcopy(current)
                changed['cases'][case][field] = value
                with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'input mismatch'):
                    suite.reference_measurements(run, changed, case)
                del changed['cases'][case][field]
                with self.subTest(missing=field), self.assertRaisesRegex(ValueError, 'input mismatch'):
                    suite.reference_measurements(run, changed, case)


if __name__ == '__main__':
    unittest.main()
