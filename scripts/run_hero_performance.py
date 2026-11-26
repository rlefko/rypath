#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Serial baseline/candidate hero timing. Caller must ensure GPU quiescence.

Defaults: native 1920x1080, 8 warmup + 96 measured frames, three paired seeds,
static and scene-specific camera motion. Captures are written after frame timing.
Use --modes static --frames N --warmup N --seeds N for a bounded screen.
"""
import argparse
import json
import math
import os
import platform
from pathlib import Path
import signal
import statistics
import struct
import subprocess
import sys
import time

from run_pipeline_benchmarks import (Cancellation, Cancelled, identity, publish, reap,
                                     reject_constant, utc)

SCENES = ('cornell', 'prism', 'pavilion-day', 'pavilion-night')
ARMS = ('baseline', 'candidate')
ENV = {'MTL_DEBUG_LAYER': '0', 'MTL_SHADER_VALIDATION': '0', 'MTL_CAPTURE_ENABLED': '0'}


def integer(value, name):
    if type(value) is not int or value < 0:
        raise ValueError(f'{name} must be a nonnegative integer')
    return value


def finite(value, name, positive=False):
    if type(value) not in (int, float) or not math.isfinite(value) or value < 0 or (positive and value == 0):
        raise ValueError(f'{name} must be finite and {"positive" if positive else "nonnegative"}')
    return value


def f32(value):
    return struct.unpack('f', struct.pack('f', value))[0]


def plan_runs(a):
    plan = []
    pair = 0
    for seed_index, seed in enumerate(a.seeds):
        scenes = SCENES[seed_index % 4:] + SCENES[:seed_index % 4]
        for scene in scenes:
            for mode_index, mode in enumerate(a.modes):
                order = ARMS if (seed_index + SCENES.index(scene) + mode_index) % 2 == 0 else ARMS[::-1]
                for arm in order:
                    plan.append(dict(scene=scene, mode=mode, seed=seed, arm=arm,
                                     pair=pair, pair_order=list(order)))
                pair += 1
    return plan


def settings(run):
    return dict(width=1920, height=1080, internal_width=1920, internal_height=1080,
                output_width=1920, output_height=1080, seed=run['seed'], material_seed=0,
                resource_profile='large' if run['scene'].startswith('pavilion') else 'standard',
                execution='wavefront', intersection_profile='native', target_scale=1,
                estimator='area-replay', spectral_transport='hero', hero_wavelengths='visible',
                requested_hero_wavelengths='visible', requested_area_mutation_policy='spectral-refresh',
                requested_area_stencil='reciprocal16', effective_mutation_steps=1, mutation_steps=1,
                reconstruction='temporal', variance_policy='conservative', variance_prefilter=False,
                strict_spatial_segments=False, guide_cache=True, footprint_samples=1,
                area_contract=6, area_output='conditional-mean', area_target='quarter-l1-v1',
                area_source_mis='source-target-mis-v1', area_temporal_reuse=20,
                capacity=65536, effective_capacity=65536, quantum=8, group_width=32,
                packet_events=1, packet_layout='packed', profile_stages=False,
                shadow_traversal_requested='query', shadow_traversal_effective='query',
                as_build='default', capture_frame=None, capture_path='')


def command(a, run, report, capture_paths):
    expected = settings(run)
    cmd = [str(getattr(a, run['arm'])), '--scene', str(a.scenes[run['scene']]),
           '--output', str(report), '--frames', str(a.frames), '--warmup', str(a.warmup)]
    options = {'width': 1920, 'height': 1080, 'output-width': 1920, 'output-height': 1080,
               'seed': run['seed'], 'material-seed': 0, 'execution': 'wavefront',
               'intersections': 'native', 'shadow-traversal': 'query', 'as-build': 'default',
               'target-scale': 1, 'resource-profile': expected['resource_profile'],
               'estimator': 'area-replay', 'mutations': 1, 'area-output': 'conditional-mean',
               'spectral-transport': 'hero', 'hero-wavelengths': 'visible',
               'area-mutation-policy': 'spectral-refresh', 'area-stencil': 'reciprocal16',
               'reconstruction': 'temporal', 'variance-policy': 'conservative',
               # Conservative policy rejects any explicit prefilter flag, even off.
               # Its default is off; inspect_report still verifies the effective value.
               'strict-spatial-segments': 'off', 'guide-cache': 'on',
               'capacity': 65536, 'quantum': 8, 'group-width': 32,
               'packet-events': 1, 'packet-layout': 'packed'}
    for key, value in options.items():
        cmd += ['--' + key, str(value)]
    if run['mode'] == 'motion':
        cmd += ['--motion-offset', str(a.offsets[run['scene']])]
    if capture_paths:
        cmd += ['--capture', str(capture_paths[0]), '--reconstructed-capture', str(capture_paths[1])]
    return cmd


def inspect_report(report, expected, frames, warmup, offset):
    if report.get('status') != 'complete' or integer(report.get('exit_code'), 'exit_code') != 0:
        raise ValueError(f"Benchmark failed: {report.get('diagnostic')}")
    for key, value in expected.items():
        actual = report['settings'].get(key)
        if (actual != value or (type(value) is bool and type(actual) is not bool) or
                (type(value) is int and key != 'target_scale' and type(actual) is not int) or
                (key == 'target_scale' and type(actual) not in (int, float))):
            raise ValueError(f'Report setting mismatch: {key}')
    for key, value in dict(requested_frames=frames, completed_frames=frames,
                           requested_warmup=warmup, completed_warmup=warmup,
                           consumed_sample_indices=frames+warmup).items():
        if integer(report.get(key), key) != value:
            raise ValueError(f'Report count mismatch: {key}')
    if report.get('capture_in_frame_timing') is not False:
        raise ValueError('Capture timing exclusion must be explicit')
    if report.get('motion_offset') != offset and not (offset is not None and
            type(report.get('motion_offset')) in (int, float) and
            math.isclose(report['motion_offset'], offset, rel_tol=1e-8, abs_tol=1e-12)):
        raise ValueError('Motion offset mismatch')
    if not isinstance(report.get('source_identity'), str) or not report['source_identity']:
        raise ValueError('Missing scene/asset source identity')
    if len(report['frames']) != frames+warmup:
        raise ValueError('Missing frame observations')
    pixels = expected['width'] * expected['height']
    times = []
    observed = {name: {kind: [] for kind in ('cpu_wall', 'GPU')}
                for name in ('summary', 'pipeline_summary')}
    for i, row in enumerate(report['frames']):
        if (row.get('complete') is not True or row.get('pipeline_complete') is not True or
                integer(row.get('sample_index'), 'sample_index') != i or
                row.get('warmup') is not (i < warmup) or
                integer(row.get('completed_samples'), 'completed_samples') != pixels):
            raise ValueError(f'Incomplete/mismatched frame {i}')
        if type(row.get('history_used')) is not bool:
            raise ValueError('Missing history accounting')
        logical = pixels*((7 if row['history_used'] else 5)+1)
        actual = integer(row.get('completed_evaluations'), 'completed_evaluations')
        cached = integer(row.get('cached_evaluations', 0), 'cached_evaluations')
        nominal = integer(row.get('logical_evaluations', logical), 'logical_evaluations')
        if actual+cached != logical or nominal != logical:
            raise ValueError(f'Evaluation accounting mismatch at frame {i}')
        if integer(row.get('mutation_attempts'), 'mutation_attempts') != pixels:
            raise ValueError('Mutation count mismatch')
        if row.get('area_mutation_policy') != 'spectral-refresh' or row.get('area_stencil') != 'reciprocal16':
            raise ValueError('Effective mutation policy/stencil mismatch')
        phase, motion = 'static', 0
        if offset is not None:
            if i < warmup:
                phase = 'warmup'
            else:
                index, quarter = i-warmup, frames//4
                if index < quarter:
                    phase = 'hold-start'
                elif index < 2*quarter:
                    phase, motion = 'translate', f32(offset*(index-quarter+1)/quarter)
                elif index < 3*quarter:
                    phase, motion = 'hold-end', offset
                else:
                    phase = 'cut-start'
        if row.get('phase') != phase or not math.isclose(row.get('motion_offset', math.inf), motion,
                                                        rel_tol=1e-6, abs_tol=1e-8):
            raise ValueError(f'Motion schedule mismatch at frame {i}')
        finite(row.get('cpu_wall_seconds'), 'raw frame seconds', True)
        elapsed = finite(row.get('pipeline_cpu_wall_seconds'), 'pipeline frame seconds', True)
        for field in ('gpu_seconds', 'pipeline_gpu_seconds'):
            if row.get(field) is not None:
                finite(row[field], field)
        if i >= warmup:
            times.append(elapsed)
            observed['summary']['cpu_wall'].append(row['cpu_wall_seconds'])
            observed['pipeline_summary']['cpu_wall'].append(elapsed)
            for name, field in (('summary', 'gpu_seconds'), ('pipeline_summary', 'pipeline_gpu_seconds')):
                if row.get(field) is not None:
                    observed[name]['GPU'].append(row[field])
    if len(observed['summary']['GPU']) != frames:
        raise ValueError('Measured raw frames require finite GPU timestamps')
    # Reconstruction creation may omit its GPU probe at absolute frame zero.
    if len(observed['pipeline_summary']['GPU']) != frames - (1 if warmup == 0 else 0):
        raise ValueError('Missing measured complete-pipeline GPU timestamps')
    metric = report['pipeline_summary']['cpu_wall']
    ordered = sorted(times)
    median, p95 = ordered[(frames-1)//2], ordered[math.ceil(.95*frames)-1]
    for name in ('summary', 'pipeline_summary'):
        for kind in ('cpu_wall', 'GPU'):
            m = report[name][kind]
            count = integer(m.get('count'), name+'.'+kind+'.count')
            required = len(observed[name][kind])
            if count != required:
                raise ValueError('Summary observation count mismatch')
            for field in ('median_seconds', 'p95_seconds'):
                if count:
                    finite(m.get(field), name+'.'+kind+'.'+field)
            values = sorted(observed[name][kind])
            if values:
                for key, value in (('median_seconds', values[(count-1)//2]),
                                   ('p95_seconds', values[math.ceil(.95*count)-1])):
                    if not math.isclose(m[key], value, rel_tol=1e-10, abs_tol=1e-12):
                        raise ValueError('Summary disagrees with actual frame observations')
            elif m.get('median_seconds') is not None or m.get('p95_seconds') is not None:
                raise ValueError('Empty GPU summary must use null statistics')
    for key, value in (('median_seconds', median), ('p95_seconds', p95)):
        if not math.isclose(metric[key], value, rel_tol=1e-10, abs_tol=1e-12):
            raise ValueError('Reported pipeline summary disagrees with frame observations')
    wall = sum(times)
    fps = frames/wall
    for key, value in (('measured_wall_seconds', wall), ('frames_per_second', fps)):
        if not math.isclose(finite(report['pipeline_summary'].get(key), key, True), value, rel_tol=1e-10):
            raise ValueError(f'Pipeline {key} disagrees with frame observations')
    return dict(source_identity=report['source_identity'], build=report.get('build'),
                device=report.get('device'), settings=report['settings'],
                fps=fps, median_seconds=median, p95_seconds=p95,
                summary=report['summary'], pipeline_summary=report['pipeline_summary'])


def paired_summary(runs):
    result = []
    for scene in SCENES:
        for mode in ('static', 'motion'):
            rows = [r for r in runs if r['scene'] == scene and r['mode'] == mode and r['complete']]
            pairs = {}
            for row in rows:
                pairs.setdefault(row['seed'], {})[row['arm']] = row
            comparisons = []
            for seed, arms in pairs.items():
                if set(arms) != set(ARMS):
                    continue
                b, c = (arms[arm]['benchmark'] for arm in ARMS)
                comparisons.append(dict(seed=seed, actual_order=arms['baseline']['pair_order'],
                    baseline={k:b[k] for k in ('fps','median_seconds','p95_seconds')},
                    candidate={k:c[k] for k in ('fps','median_seconds','p95_seconds')},
                    candidate_over_baseline={k:c[k]/b[k] for k in ('fps','median_seconds','p95_seconds')}))
            if comparisons:
                result.append(dict(scene=scene, mode=mode, pairs=comparisons,
                    median_paired_ratios={k:statistics.median(p['candidate_over_baseline'][k] for p in comparisons)
                                          for k in ('fps','median_seconds','p95_seconds')}))
    return result


def arguments(argv):
    p = argparse.ArgumentParser(description=__doc__)
    for arm in ARMS:
        p.add_argument('--'+arm, type=Path, required=True)
        p.add_argument('--'+arm+'-revision', help='Optional supplied source revision; hashes bind actual artifacts')
    for scene in SCENES:
        p.add_argument('--'+scene, type=Path, required=True)
    p.add_argument('--motion-offset', action='append', default=[], metavar='SCENE=FLOAT')
    p.add_argument('--modes', default='static,motion')
    p.add_argument('--frames', type=int, default=96)
    p.add_argument('--warmup', type=int, default=8)
    p.add_argument('--seeds', default='31001,31002,31003')
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--skip-captures', action='store_true', help='Explicit bounded timing screen; no final images')
    a = p.parse_args(argv)
    try:
        if not hasattr(os, 'wait4'):
            raise ValueError('macOS/Linux wait4 required')
        a.modes = a.modes.split(',')
        if not a.modes or len(set(a.modes)) != len(a.modes) or set(a.modes)-{'static','motion'}:
            raise ValueError('Modes must be static and/or motion, without duplicates')
        if not (0 < a.frames < 2**64 and 0 <= a.warmup < 2**64-a.frames):
            raise ValueError('Invalid frame/warmup count')
        if 'motion' in a.modes and (a.frames < 8 or a.frames % 4):
            raise ValueError('Motion requires at least eight frames divisible by four')
        tokens = a.seeds.split(',')
        if not all(s.isascii() and s.isdecimal() for s in tokens):
            raise ValueError('Seeds must be unique unsigned decimal integers')
        a.seeds = [int(s) for s in tokens]
        if len(set(a.seeds)) != len(a.seeds) or any(s >= 2**64 for s in a.seeds):
            raise ValueError('Seeds must be unique uint64 values')
        a.scenes = {name:getattr(a,name.replace('-','_')).expanduser().resolve(strict=True) for name in SCENES}
        for arm in ARMS:
            executable = getattr(a,arm).expanduser().resolve(strict=True)
            if not executable.is_file() or not os.access(executable,os.X_OK):
                raise ValueError('Executable must be an executable regular file')
            setattr(a,arm,executable)
            if not (executable.parent/'rypath.metallib').is_file():
                raise ValueError('Each executable requires adjacent rypath.metallib')
        if any(not path.is_file() for path in a.scenes.values()):
            raise ValueError('Scene inputs must be regular files')
        a.offsets = {}
        for item in a.motion_offset:
            name, sep, raw = item.partition('=')
            if not sep or name not in SCENES or name in a.offsets:
                raise ValueError('Motion offsets require unique SCENE=FLOAT values')
            value = f32(float(raw))
            if not math.isfinite(value) or value == 0:
                raise ValueError('Motion offsets must be finite nonzero binary32 values')
            a.offsets[name] = value
        if 'motion' in a.modes and set(a.offsets) != set(SCENES):
            raise ValueError('Motion requires an explicit offset for each of the four scenes')
        a.output_dir = a.output_dir.expanduser().absolute()
        if os.path.lexists(a.output_dir) or not a.output_dir.parent.is_dir():
            raise ValueError('Output directory must be fresh with an existing parent')
        a.output_dir = a.output_dir.parent.resolve()/a.output_dir.name
    except (OSError, ValueError, OverflowError) as error:
        p.error(str(error))
    return a


def main(argv=None):
    a = arguments(argv)
    plan = plan_runs(a)
    a.output_dir.mkdir()
    cancellation = Cancellation()
    previous = {s:signal.getsignal(s) for s in (signal.SIGINT,signal.SIGTERM)}
    manifest = dict(schema=1, complete=False, started=utc(), runs=[], plan=plan,
        requested_frames=a.frames, requested_warmup=a.warmup, motion_offsets=a.offsets,
        environment_overrides=ENV, captures_enabled=not a.skip_captures,
        timing_scope='complete pipeline frame CPU wall, excluding warmup/startup and post-loop captures',
        quiescence='Caller must ensure no competing GPU work; not verified',
        artifact_binding='executable and adjacent metallib SHA-256 before and after each run')
    code = 1
    try:
        for s in previous:
            signal.signal(s,cancellation.request)
        manifest['artifacts'] = {arm:dict(binary=identity(getattr(a,arm)),
            metallib=identity(getattr(a,arm).parent/'rypath.metallib'),
            supplied_revision=getattr(a,arm+'_revision')) for arm in ARMS}
        manifest['inputs'] = {name:identity(path) for name,path in a.scenes.items()}
        manifest['platform'] = platform.platform()
        manifest['python'] = sys.version
        manifest['runner'] = identity(Path(__file__).resolve())
        manifest['helper'] = identity(Path(__file__).with_name('run_pipeline_benchmarks.py'))
        publish(a.output_dir/'manifest.json',manifest)
        sources, devices = {}, {}
        for index, run in enumerate(plan):
            cancellation.check()
            arm, scene = run['arm'], run['scene']
            artifacts = manifest['artifacts'][arm]
            def verify_artifacts():
                for item in (artifacts['binary'],artifacts['metallib'],manifest['inputs'][scene]):
                    if identity(Path(item['path'])) != item:
                        raise ValueError('Artifact/input changed during benchmark: '+item['path'])
            verify_artifacts()
            stem = f"{index:03}-{scene}-{run['mode']}-{arm}-{run['seed']}"
            report_path = a.output_dir/(stem+'.json')
            captures = [] if a.skip_captures else [a.output_dir/(stem+'.raw.exr'),
                                                   a.output_dir/(stem+'.reconstructed.exr')]
            cmd = command(a,run,report_path,captures)
            row = dict(run, command=cmd, report=str(report_path), started=utc(), complete=False,
                       active_pid=None, exit_code=None)
            manifest['runs'].append(row)
            publish(a.output_dir/'manifest.json',manifest)
            child, begin = None, time.monotonic()
            try:
                with (a.output_dir/(stem+'.stdout.log')).open('x') as out, (a.output_dir/(stem+'.stderr.log')).open('x') as err:
                    env = {k:v for k,v in os.environ.items() if not k.startswith(('MTL_','METAL_'))}
                    child = subprocess.Popen(cmd, stdout=out, stderr=err, start_new_session=True,
                                             env={**env, **ENV})
                    row.update(child_pid=child.pid,active_pid=child.pid)
                    publish(a.output_dir/'manifest.json',manifest)
                    usage, interrupted = reap(child,cancellation=cancellation)
                    row.update(active_pid=None,exit_code=child.returncode,
                               peak_rss_bytes=usage.ru_maxrss*(1024 if sys.platform=='linux' else 1))
                    if interrupted:
                        cancellation.check()
                        raise KeyboardInterrupt
                if child.returncode:
                    raise ValueError(f'Benchmark exited {child.returncode}; logs retained: {stem}')
                verify_artifacts()
                with report_path.open() as stream:
                    parsed = json.load(stream,parse_constant=reject_constant)
                offset = a.offsets[scene] if run['mode']=='motion' else None
                row['benchmark'] = inspect_report(parsed,settings(run),a.frames,a.warmup,offset)
                source = row['benchmark']['source_identity']
                # Scene preparation includes the seed in its source identity.
                # Match both arms and poses within each scene/seed pair.
                if sources.setdefault((scene, run['seed']), source) != source:
                    raise ValueError('Scene/asset source identity changed across paired runs')
                device = row['benchmark']['device']
                if not isinstance(device,dict) or not device.get('name') or not device.get('registry_id'):
                    raise ValueError('Missing GPU device identity')
                if devices.setdefault('device',device) != device:
                    raise ValueError('GPU device changed across paired runs')
                row['captures'] = []
                for path in captures:
                    if not path.is_file() or path.stat().st_size == 0:
                        raise ValueError('Missing final capture: '+str(path))
                    row['captures'].append(identity(path))
                row['complete'] = True
            finally:
                if child is not None and child.returncode is None:
                    reap(child,interrupted=True,cancellation=cancellation)
                    row['exit_code'] = child.returncode
                row.update(active_pid=None,stopped=utc(),child_wall_seconds=time.monotonic()-begin)
                publish(a.output_dir/'manifest.json',manifest)
            print(f'[{index+1}/{len(plan)}] {stem}: complete',flush=True)
        cancellation.check()
        code = 0
    except Cancelled as error:
        code = 128+error.number
        manifest['failure'] = 'Cancelled; own child signaled and reaped'
    except KeyboardInterrupt:
        code = 130
        manifest['failure'] = 'Interrupted; own child signaled and reaped'
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError) as error:
        manifest['failure'] = str(error)
    finally:
        if cancellation.number is not None:
            code = 128+cancellation.number
        manifest.update(complete=code==0,exit_code=code,stopped=utc())
        publish(a.output_dir/'manifest.json',manifest)
        publish(a.output_dir/'summary.json',dict(schema=1,complete=code==0,
            failure=manifest.get('failure'),scenes=paired_summary(manifest['runs']),
            ratio_direction='candidate / baseline; FPS above 1 faster, latency below 1 faster',
            no_cross_scene_aggregate=True))
        for s, handler in previous.items():
            signal.signal(s,handler)
    return code


if __name__ == '__main__':
    sys.exit(main())
