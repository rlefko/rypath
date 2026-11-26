#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Portable runner checks: mock reports/processes only, never GPU execution."""
import copy
import json
import math
from pathlib import Path
import signal
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import run_hero_performance as runner


def fixture(expected, frames=8, warmup=1, offset=None, cached=False):
    pixels=1920*1080
    rows=[]
    for i in range(frames+warmup):
        phase, motion='static',0
        if offset is not None:
            n,q=i-warmup,frames//4
            if i<warmup: phase='warmup'
            elif n<q: phase='hold-start'
            elif n<2*q: phase,motion='translate',runner.f32(offset*(n-q+1)/q)
            elif n<3*q: phase,motion='hold-end',offset
            else: phase='cut-start'
        logical=pixels*(6 if i==0 else 8)
        row=dict(complete=True,pipeline_complete=True,sample_index=i,warmup=i<warmup,
                 completed_samples=pixels,history_used=i!=0,completed_evaluations=logical,
                 mutation_attempts=pixels,area_mutation_policy='spectral-refresh',
                 area_stencil='reciprocal16',phase=phase,motion_offset=motion,
                 cpu_wall_seconds=.015,gpu_seconds=.01,pipeline_gpu_seconds=.015,
                 pipeline_cpu_wall_seconds=.02)
        if cached:
            row.update(completed_evaluations=logical-pixels,cached_evaluations=pixels,
                       logical_evaluations=logical)
        rows.append(row)
    metric=dict(count=frames,median_seconds=.02,p95_seconds=.02)
    return dict(status='complete',exit_code=0,settings=expected,
                requested_frames=frames,completed_frames=frames,requested_warmup=warmup,
                completed_warmup=warmup,consumed_sample_indices=frames+warmup,
                capture_in_frame_timing=False,motion_offset=offset,source_identity='scene-assets-sha',
                device=dict(name='mock-GPU',registry_id=1),build='untrusted descriptive string',
                frames=rows,summary=dict(cpu_wall=dict(count=frames,median_seconds=.015,p95_seconds=.015),
                             GPU=dict(count=frames,median_seconds=.01,p95_seconds=.01)),
                pipeline_summary=dict(cpu_wall=metric.copy(),
                    GPU=dict(count=frames,median_seconds=.015,p95_seconds=.015),
                    measured_wall_seconds=.02*frames,frames_per_second=50))


class RunnerTests(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(runner.platform, 'platform', return_value='mock-platform')
        patcher.start()
        self.addCleanup(patcher.stop)
        self.expected=runner.settings(dict(scene='cornell',seed=1))

    def test_exact_and_cached_accounting(self):
        for cached in (False,True):
            result=runner.inspect_report(fixture(self.expected,cached=cached),self.expected,8,1,None)
            self.assertEqual(result['fps'],50)

    def test_reject_corrupt_counts_settings_times_and_motion(self):
        changes=[lambda r:r['frames'][0].update(completed_evaluations=True),
                 lambda r:r['frames'][0].update(cached_evaluations=-1),
                 lambda r:r['frames'][0].update(logical_evaluations=0),
                 lambda r:r['frames'][0].update(mutation_attempts=0),
                 lambda r:r['frames'][1].update(pipeline_cpu_wall_seconds=math.nan),
                 lambda r:r['frames'][1].update(pipeline_gpu_seconds=math.inf),
                 lambda r:r['frames'][1].update(area_stencil='adjacent'),
                 lambda r:r['settings'].update(capacity=32),
                 lambda r:r.update(completed_frames=True),
                 lambda r:r.update(capture_in_frame_timing=True),
                 lambda r:r['pipeline_summary']['cpu_wall'].update(p95_seconds=.1),
                 lambda r:r['frames'][4].update(phase='static')]
        for change in changes:
            report=fixture(copy.deepcopy(self.expected),offset=.3)
            change(report)
            with self.assertRaises((ValueError,TypeError)):
                runner.inspect_report(report,self.expected,8,1,.3)

    def test_motion_and_alternation(self):
        runner.inspect_report(fixture(self.expected,offset=.3),self.expected,8,1,.3)
        a=SimpleNamespace(seeds=[1,2,3],modes=['static','motion'])
        plan=runner.plan_runs(a)
        self.assertEqual(len(plan),48)
        self.assertEqual([r['arm'] for r in plan[:4]],['baseline','candidate','candidate','baseline'])
        self.assertTrue(all(plan[i]['pair']==plan[i+1]['pair'] for i in range(0,len(plan),2)))

    def setup_paths(self, directory, modes='static,motion'):
        d=Path(directory)
        args=[]
        for arm in runner.ARMS:
            (d/arm).mkdir()
            executable=d/arm/'benchmark'
            executable.write_text('#!/bin/sh\nexit 0\n')
            executable.chmod(0o755)
            (d/arm/'rypath.metallib').write_bytes(arm.encode())
            args += ['--'+arm,str(executable)]
        for scene in runner.SCENES:
            path=d/(scene+'.pbrt'); path.write_text('WorldBegin\n')
            args += ['--'+scene,str(path),'--motion-offset',scene+'=.3']
        return args+['--output-dir',str(d/'output'),'--modes',modes,
                     '--frames','8','--warmup','1','--seeds','1']

    def test_conservative_command_uses_prefilter_default_but_verifies_report(self):
        a=SimpleNamespace(baseline=Path('/baseline/benchmark'),frames=1,warmup=1,
                          scenes={'cornell':Path('/scenes/cornell.pbrt')})
        run=dict(scene='cornell',mode='static',seed=1,arm='baseline')
        cmd=runner.command(a,run,Path('/output/report.json'),[])
        self.assertNotIn('--variance-prefilter',cmd)
        self.assertEqual(cmd[cmd.index('--variance-policy')+1],'conservative')
        self.assertIs(runner.settings(run)['variance_prefilter'],False)
        report=fixture(copy.deepcopy(self.expected))
        report['settings']['variance_prefilter']=True
        with self.assertRaisesRegex(ValueError,'variance_prefilter'):
            runner.inspect_report(report,self.expected,8,1,None)

    def test_zero_warmup_temporal_creation_omits_only_first_pipeline_gpu(self):
        report=fixture(copy.deepcopy(self.expected),warmup=0)
        report['frames'][0]['pipeline_gpu_seconds']=None
        report['pipeline_summary']['GPU']['count']=7
        runner.inspect_report(report,self.expected,8,0,None)
        report['frames'][1]['pipeline_gpu_seconds']=None
        with self.assertRaisesRegex(ValueError,'Missing measured complete-pipeline GPU'):
            runner.inspect_report(report,self.expected,8,0,None)

    def test_motion_offsets_and_fresh_directory_required(self):
        with tempfile.TemporaryDirectory() as d:
            args=self.setup_paths(d)
            no_offsets=[]
            for i,item in enumerate(args):
                if item=='--motion-offset' or (i and args[i-1]=='--motion-offset'): continue
                no_offsets.append(item)
            with self.assertRaises(SystemExit): runner.arguments(no_offsets)
            (Path(d)/'output').mkdir()
            with self.assertRaises(SystemExit): runner.arguments(args)

    def test_full_mocked_matrix_retains_captures_and_pairs(self):
        with tempfile.TemporaryDirectory() as d:
            args=self.setup_paths(d)
            args[args.index('--seeds')+1]='1,2,3'
            calls=[]
            def popen(cmd,**kw):
                calls.append((cmd,kw))
                def option(name): return cmd[cmd.index('--'+name)+1]
                scene=Path(option('scene')).stem
                run=dict(scene=scene,seed=int(option('seed')))
                report=fixture(runner.settings(run),offset=float(option('motion-offset'))
                               if '--motion-offset' in cmd else None,cached=True)
                report['source_identity']='scene-assets-seed-'+str(run['seed'])
                Path(option('output')).write_text(json.dumps(report))
                for flag in ('capture','reconstructed-capture'):
                    Path(option(flag)).write_bytes(b'mock EXR')
                return SimpleNamespace(pid=12345,returncode=None)
            def reap(child,**kw):
                child.returncode=0
                return SimpleNamespace(ru_maxrss=12),False
            with mock.patch.object(runner.subprocess,'Popen',side_effect=popen),\
                 mock.patch.object(runner,'reap',side_effect=reap),\
                 mock.patch.dict(runner.os.environ,{'MTL_SHADER_VALIDATION':'1','METAL_CAPTURE':'1'}):
                self.assertEqual(runner.main(args),0)
            manifest=json.loads((Path(d)/'output/manifest.json').read_text())
            summary=json.loads((Path(d)/'output/summary.json').read_text())
            self.assertTrue(manifest['complete'])
            self.assertEqual(len(manifest['runs']),48)
            self.assertTrue(all(len(scene['pairs'])==3 for scene in summary['scenes']))
            self.assertEqual(len(summary['scenes']),8)
            self.assertTrue(all(len(r['captures'])==2 for r in manifest['runs']))
            self.assertTrue(all(kw['env']['MTL_SHADER_VALIDATION']=='0' and
                                'METAL_CAPTURE' not in kw['env'] for _,kw in calls))
            self.assertTrue(all(s['median_paired_ratios']['fps']==1 for s in summary['scenes']))

    def test_failure_stops_and_retains_failed_run(self):
        with tempfile.TemporaryDirectory() as d:
            args=self.setup_paths(d,'static')
            child=SimpleNamespace(pid=12345,returncode=None)
            def reap(*a,**kw):
                child.returncode=7
                return SimpleNamespace(ru_maxrss=12),False
            with mock.patch.object(runner.subprocess,'Popen',return_value=child) as spawn,\
                 mock.patch.object(runner,'reap',side_effect=reap):
                self.assertEqual(runner.main(args),1)
            self.assertEqual(spawn.call_count,1)
            manifest=json.loads((Path(d)/'output/manifest.json').read_text())
            self.assertFalse(manifest['complete'])
            self.assertEqual(manifest['runs'][0]['exit_code'],7)
            self.assertIsNone(manifest['runs'][0]['active_pid'])
            self.assertTrue(list((Path(d)/'output').glob('*.stderr.log')))

    def test_artifact_mutation_and_source_mismatch_stop_matrix(self):
        for kind in ('artifact', 'source', 'capture'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as d:
                args=self.setup_paths(d,'static')
                calls=[]
                def popen(cmd,**kw):
                    calls.append(cmd)
                    def option(name): return cmd[cmd.index('--'+name)+1]
                    run=dict(scene=Path(option('scene')).stem,seed=int(option('seed')))
                    report=fixture(runner.settings(run))
                    if kind=='source' and len(calls)==2:
                        report['source_identity']='different-assets'
                    Path(option('output')).write_text(json.dumps(report))
                    if kind!='capture':
                        for flag in ('capture','reconstructed-capture'):
                            Path(option(flag)).write_bytes(b'mock EXR')
                    if kind=='artifact':
                        Path(cmd[0]).write_text('changed executable')
                    return SimpleNamespace(pid=12345,returncode=None)
                def reap(child,**kw):
                    child.returncode=0
                    return SimpleNamespace(ru_maxrss=12),False
                with mock.patch.object(runner.subprocess,'Popen',side_effect=popen),\
                     mock.patch.object(runner,'reap',side_effect=reap):
                    self.assertEqual(runner.main(args),1)
                self.assertEqual(len(calls),2 if kind=='source' else 1)
                manifest=json.loads((Path(d)/'output/manifest.json').read_text())
                self.assertFalse(manifest['complete'])
                self.assertTrue(manifest['failure'])

    def test_cancellation_reaps_only_owned_child(self):
        with tempfile.TemporaryDirectory() as d:
            args=self.setup_paths(d,'static')
            child=SimpleNamespace(pid=12345,returncode=None)
            def reap(*args,**kw):
                self.assertIs(args[0],child)
                kw['cancellation'].request(signal.SIGTERM,None)
                child.returncode=-signal.SIGTERM
                return SimpleNamespace(ru_maxrss=12),True
            with mock.patch.object(runner.subprocess,'Popen',return_value=child),\
                 mock.patch.object(runner,'reap',side_effect=reap):
                self.assertEqual(runner.main(args),128+signal.SIGTERM)
            manifest=json.loads((Path(d)/'output/manifest.json').read_text())
            self.assertFalse(manifest['complete'])
            self.assertIsNone(manifest['runs'][0]['active_pid'])


if __name__=='__main__': unittest.main()
