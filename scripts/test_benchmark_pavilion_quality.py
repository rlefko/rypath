#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import benchmark_pavilion_quality as benchmark

TIME = '        1.25 real         2.00 user         0.10 sys\n       123456 maximum resident set size\n'
PHASES = 'Preparation seconds: 0.2\nPipeline seconds: 0.3\nRender and accumulation seconds: 0.5\n'
SOURCE = (b'Film "rgb" "float iso" [500] "float maxcomponentvalue" [50]\n'
          b'"integer xresolution" [1600] "integer yresolution" [850]\n'
          b'Camera "perspective" "float lensradius" [.0125]\n'
          b'Sampler "halton" "integer pixelsamples" [256]\nWorldBegin\n')


class TimingTests(unittest.TestCase):
    def test_parse_process_time_and_peak_bytes(self):
        self.assertEqual(benchmark.parse_time(TIME), {'time_real_seconds': 1.25,
                         'user_seconds': 2., 'system_seconds': .1, 'peak_rss_bytes': 123456})
        for bad in ('', TIME + TIME, TIME.replace('123456', '0'), TIME.replace('1.25', 'nan')):
            with self.subTest(text=bad), self.assertRaises(ValueError):
                benchmark.parse_time(bad)

    def test_candidate_phases_and_explicit_unavailable(self):
        phases = benchmark.parse_phases(PHASES, 'candidate')
        self.assertEqual([phases[p] for p in benchmark.PHASES], [.2, .3, .5])
        for backend in ('baseline', 'pbrt'):
            self.assertTrue(all(benchmark.parse_phases(PHASES, backend)[p] is None
                                for p in benchmark.PHASES))
        for bad in ('', PHASES + PHASES, PHASES.replace('0.2', 'nan'), PHASES.replace('0.2', '-1')):
            with self.subTest(text=bad), self.assertRaises(ValueError):
                benchmark.parse_phases(bad, 'candidate')

    def test_blocking_execution_uses_per_process_time_file(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            calls = []
            def runner(command, **kwargs):
                calls.append(command)
                self.assertTrue(kwargs['check'])
                kwargs['stdout'].write(PHASES.encode())
                Path(command[command.index('-o') + 1]).write_text(TIME)
            ticks = iter([100, 1250000100])
            result = benchmark.execute(['renderer', 'scene'], directory / 'run.log', directory / 'run.time',
                                       cwd=directory, timeout=10, runner=runner, clock=lambda: next(ticks))
            self.assertEqual(calls[0][:4], ['/usr/bin/time', '-l', '-o', str(directory / 'run.time')])
            self.assertEqual(result['wall_nanoseconds'], 1250000000)
            self.assertEqual(result['peak_rss_bytes'], 123456)
            def fail(command, **kwargs):
                raise subprocess.CalledProcessError(7, command)
            with self.assertRaises(subprocess.CalledProcessError):
                benchmark.execute(['renderer'], directory / 'bad.log', directory / 'bad.time',
                                  cwd=directory, timeout=10, runner=fail)

    def test_timeout_kills_measurement_and_renderer_process_group(self):
        process = MagicMock()
        process.pid = 42
        process.wait.side_effect = [subprocess.TimeoutExpired(['time', 'renderer'], 1), 0]
        context = MagicMock()
        context.__enter__.return_value = process
        with patch.object(benchmark.subprocess, 'Popen', return_value=context) as popen, \
                patch.object(benchmark.os, 'killpg') as kill:
            with self.assertRaises(subprocess.TimeoutExpired):
                benchmark.blocking_run(['time', 'renderer'], stdout=None, stderr=None,
                                       cwd='.', timeout=1, check=True)
            self.assertTrue(popen.call_args.kwargs['start_new_session'])
            kill.assert_called_once_with(42, benchmark.signal.SIGKILL)
            self.assertEqual(process.wait.call_count, 2)


    def test_frozen_budgets(self):
        protocol = benchmark.quality.load_protocol()
        args = argparse.Namespace(scene=None, spp=None, backend=None)
        cases = set(benchmark.cases(protocol, args))
        self.assertEqual(len(cases), 12)
        self.assertEqual({spp for b, s, spp in cases if s == 'day'}, {64, 256})
        self.assertEqual({spp for b, s, spp in cases if s == 'night'}, {64, 512})
        args.scene, args.spp = ['night'], [256]
        with self.assertRaisesRegex(ValueError, 'budget'):
            list(benchmark.cases(protocol, args))

    def test_one_warmup_three_serial_repetitions_excludes_warmup(self):
        protocol = benchmark.quality.load_protocol()
        with tempfile.TemporaryDirectory() as directory:
            args = argparse.Namespace(scene=None, spp=None, backend=None, output_dir=Path(directory))
            calls = []
            def run(args, protocol, staged, identity, backend, scene, spp, label, repetition):
                calls.append((backend, scene, spp, label, repetition))
                return {'label': label, 'repetition': repetition,
                        'total_raw_and_clamped_seconds': 100 if label == 'warmup' else repetition}
            result = benchmark.benchmark(args, protocol, Path(directory),
                                         dict.fromkeys(benchmark.quality.BACKENDS, {}), execute_case=run)
            self.assertEqual(len(calls), 48)
            for start in range(0, len(calls), 4):
                block = calls[start:start+4]
                self.assertEqual(len({c[:3] for c in block}), 1)
                self.assertEqual([c[3:] for c in block],
                                 [('warmup', 0), ('measured', 1), ('measured', 2), ('measured', 3)])
            self.assertTrue(result['complete_protocol'])
            self.assertTrue(all(c['mean_total_seconds'] == 2 for c in result['cases']))

    def run_case_fixture(self, backend, executor):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = root / 'staged/barcelona-pavilion'
        source.mkdir(parents=True)
        (source / 'pavilion-day.pbrt').write_bytes(SOURCE)
        args = argparse.Namespace(output_dir=root / 'output', timeout=10)
        return benchmark.run_once(args, benchmark.quality.load_protocol(), root / 'staged',
                                  {'binary': '/renderer'}, backend, 'day', 64, 'measured', 1,
                                  executor=executor), root

    @staticmethod
    def fake_executor(calls):
        def execute(command, log, timing, **kwargs):
            calls.append(command)
            log.write_text(PHASES)
            timing.write_text(TIME)
            output_flag = '--outfile' if '--outfile' in command else '--output'
            Path(command[command.index(output_flag)+1]).write_bytes(b'FLOAT EXR fixture')
            if '--raw-output' in command:
                Path(command[command.index('--raw-output')+1]).write_bytes(b'raw FLOAT EXR fixture')
            return {'log': str(log), 'wall_seconds': 2 if output_flag == '--outfile' else 3,
                    'peak_rss_bytes': 456, 'command': command}
        return execute

    def test_pbrt_companion_is_separately_timed_and_transport_identical(self):
        calls = []
        record, _ = self.run_case_fixture('pbrt', self.fake_executor(calls))
        self.assertEqual(len(calls), 2)
        for command in calls:
            self.assertEqual(command[command.index('--nthreads')+1], '16')
            self.assertEqual(command[command.index('--seed')+1], '0')
            self.assertEqual(command[command.index('--render-coord-sys')+1], 'world')
        self.assertEqual(record['total_raw_and_clamped_seconds'], 4)
        self.assertEqual(record['raw_companion_seconds'], 2)
        self.assertEqual(record['artifacts']['raw']['scale'], .2)
        self.assertEqual(len({v['transport_sha256'] for v in record['variants']}), 1)
        self.assertEqual([p['plane'] for p in record['processes']], ['clamped', 'raw'])
        self.assertIsNone(record['processes'][0]['phases']['pipeline'])

    def test_candidate_writes_both_planes_once_and_baseline_keeps_historical_policy(self):
        for backend in ('candidate', 'baseline'):
            calls = []
            record, _ = self.run_case_fixture(backend, self.fake_executor(calls))
            self.assertEqual(len(calls), 1)
            self.assertEqual(set(record['artifacts']), {'raw', 'clamped'})
            self.assertEqual(record['total_raw_and_clamped_seconds'], 3)
            self.assertEqual('--material-seed' in calls[0], backend == 'candidate')
            self.assertIsNone(record['raw_companion_seconds'])

    def test_failed_execution_retains_failed_receipt(self):
        def fail(command, log, timing, **kwargs):
            raise subprocess.CalledProcessError(4, command)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'staged/barcelona-pavilion'
            source.mkdir(parents=True)
            (source / 'pavilion-day.pbrt').write_bytes(SOURCE)
            args = argparse.Namespace(output_dir=root / 'output', timeout=10)
            with self.assertRaises(subprocess.CalledProcessError):
                benchmark.run_once(args, benchmark.quality.load_protocol(), root / 'staged',
                                   {'binary': '/renderer'}, 'candidate', 'day', 64,
                                   'measured', 1, executor=fail)
            record = json.loads(next(args.output_dir.rglob('record.json')).read_text())
            self.assertEqual(record['status'], 'failed')
            self.assertIn('error', record)
            self.assertFalse((args.output_dir / 'timing-summary.json').exists())


if __name__ == '__main__':
    unittest.main()
