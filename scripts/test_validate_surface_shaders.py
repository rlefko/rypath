"""Regression checks for shader diagnostic evidence after unsuccessful reruns."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import validate_surface_shaders as diagnostic


class ShaderEvidenceTests(unittest.TestCase):
    def run_failure(self, outcome, expected_exception):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            output = build / 'surface-shader-validation'
            output.mkdir()
            results = output / 'results.json'
            results.write_text(json.dumps([{'status': 'passed'}] * len(diagnostic.TESTS)))
            listing = {'tests': [{'name': 'metal.' + name, 'command': ['probe'],
                                  'properties': []} for name in diagnostic.TESTS]}
            with patch.dict(os.environ, MTL_SHADER_VALIDATION_DEFAULT_STATE='none',
                            MTL_SHADER_VALIDATION_DISABLE_PIPELINES='probe',
                            MTL_SHADER_VALIDATION_GLOBAL_MEMORY='0',
                            MTL_SHADER_VALIDATION_ABORT_ON_FAULT='0'), \
                    patch('sys.argv', ['diagnostic', '--build-dir', directory]), \
                    patch.object(subprocess, 'check_output', return_value=json.dumps(listing)), \
                    patch.object(subprocess, 'run', side_effect=outcome) as run, \
                    self.assertRaises(expected_exception):
                diagnostic.main()
            for setting in ('MTL_SHADER_VALIDATION', 'MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING',
                            'MTL_SHADER_VALIDATION_REPORT_TO_STDERR',
                            'MTL_SHADER_VALIDATION_ABORT_ON_FAULT',
                            'MTL_SHADER_VALIDATION_GLOBAL_MEMORY',
                            'MTL_SHADER_VALIDATION_THREADGROUP_MEMORY',
                            'MTL_SHADER_VALIDATION_TEXTURE_USAGE'):
                self.assertEqual(run.call_args.kwargs['env'][setting], '1')
            self.assertEqual(run.call_args.kwargs['env']['MTL_SHADER_VALIDATION_DEFAULT_STATE'], 'all')
            self.assertNotIn('MTL_SHADER_VALIDATION_DISABLE_PIPELINES', run.call_args.kwargs['env'])
            records = json.loads(results.read_text())
            self.assertEqual(len(records), 1)
            self.assertEqual(records[0]['test'], 'metal.texture_graph')
            self.assertEqual(records[0]['command'], ['probe'])
            self.assertGreaterEqual(records[0]['seconds'], 0)
            return records[0]

    def test_timeout_replaces_previous_success(self):
        record = self.run_failure(subprocess.TimeoutExpired(['probe'], 600),
                                  subprocess.TimeoutExpired)
        self.assertEqual(record['status'], 'timeout')
        self.assertNotIn('returncode', record)

    def test_hardware_skip_is_recorded_as_failure(self):
        record = self.run_failure([subprocess.CompletedProcess(['probe'], 77)], RuntimeError)
        self.assertEqual(record['status'], 'failed')
        self.assertEqual(record['returncode'], 77)


if __name__ == '__main__':
    unittest.main()
