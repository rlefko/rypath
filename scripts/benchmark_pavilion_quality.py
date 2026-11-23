#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Serial Pavilion timings: one labelled warmup and three measured repetitions.

Run only after other validation jobs finish. PBRT's raw Film companion is a
separate process; its cost is retained alongside the clamped process and total.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import signal
import statistics
import subprocess
import time

import compare_pavilion_quality as quality

PHASES = {'preparation': 'Preparation seconds:', 'pipeline': 'Pipeline seconds:',
          'render': 'Render and accumulation seconds:'}


def parse_time(text):
    rss = re.findall(r'^\s*(\d+)\s+maximum resident set size\s*$', text, re.M)
    elapsed = re.findall(r'^\s*([0-9]+(?:\.[0-9]+)?)\s+real\s+'
                         r'([0-9]+(?:\.[0-9]+)?)\s+user\s+'
                         r'([0-9]+(?:\.[0-9]+)?)\s+sys\s*$', text, re.M)
    if len(rss) != 1 or len(elapsed) != 1 or int(rss[0]) <= 0:
        raise ValueError('missing or ambiguous macOS time/RSS measurement')
    values = tuple(float(v) for v in elapsed[0])
    if any(not math.isfinite(v) or v < 0 for v in values):
        raise ValueError('invalid process times')
    return {'time_real_seconds': values[0], 'user_seconds': values[1],
            'system_seconds': values[2], 'peak_rss_bytes': int(rss[0])}


def parse_phases(text, backend):
    if backend != 'candidate':
        return {**dict.fromkeys(PHASES), 'availability':
                'PBRT has no matching CLI phase telemetry; pipeline not applicable'
                if backend == 'pbrt' else 'frozen baseline has no CLI phase telemetry'}
    result = {}
    for name, prefix in PHASES.items():
        matches = [line[len(prefix):].strip() for line in text.splitlines()
                   if line.startswith(prefix)]
        if len(matches) != 1:
            raise ValueError('missing or ambiguous candidate phase: ' + name)
        value = float(matches[0])
        if not math.isfinite(value) or value < 0:
            raise ValueError('invalid candidate phase: ' + name)
        result[name] = value
    result['availability'] = 'reported by candidate CLI; not inferred from wall time'
    return result


def blocking_run(command, *, stdout, stderr, cwd, timeout, check):
    # /usr/bin/time is the direct child. On cancellation kill its renderer too,
    # so a failed measurement cannot leave work running behind later timings.
    with subprocess.Popen(command, stdout=stdout, stderr=stderr, cwd=cwd,
                          start_new_session=True) as process:
        try:
            code = process.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            raise
    if check and code:
        raise subprocess.CalledProcessError(code, command)


def execute(command, log, timing, *, cwd, timeout, runner=None, clock=None):
    """A blocking subprocess with per-process RSS, never cumulative ru_maxrss."""
    runner = runner or blocking_run
    clock = clock or time.perf_counter_ns
    wrapped = ['/usr/bin/time', '-l', '-o', str(timing), *command]
    start = clock()
    with log.open('wb') as stream:
        runner(wrapped, stdout=stream, stderr=subprocess.STDOUT, cwd=cwd,
               timeout=timeout, check=True)
    elapsed_ns = clock() - start
    if elapsed_ns < 0:
        raise ValueError('non-monotonic process clock')
    result = parse_time(timing.read_text())
    result.update(command=command, measurement_command=wrapped,
                  wall_nanoseconds=elapsed_ns, wall_seconds=elapsed_ns / 1e9,
                  log=str(log), log_sha256=quality.sha(log),
                  timing_log=str(timing), timing_log_sha256=quality.sha(timing))
    return result


def cases(protocol, args):
    for scene in args.scene or protocol['scenes']:
        for spp in args.spp or protocol['scenes'][scene]['budgets']:
            if spp not in protocol['scenes'][scene]['budgets']:
                raise ValueError('budget is not in the frozen protocol for ' + scene)
            for backend in args.backend or quality.BACKENDS:
                yield backend, scene, spp


def labels(protocol):
    if protocol['timing']['repetitions'] != 3 or protocol['timing']['parallel_validation']:
        raise ValueError('timing requires three serial repetitions')
    return [('warmup', 0)] + [('measured', n) for n in range(1, 4)]


def run_once(args, protocol, staged, identity, backend, scene, spp, label, repetition, *, executor=None):
    executor = executor or execute
    name = 'warmup' if label == 'warmup' else f'repeat-{repetition}'
    directory = args.output_dir / backend / scene / str(spp) / name
    directory.mkdir(parents=True, exist_ok=True)
    receipt = directory / 'record.json'
    if receipt.exists():
        raise ValueError('timing receipt already exists; choose a fresh output directory: ' + str(receipt))
    original = staged / 'barcelona-pavilion' / f'pavilion-{scene}.pbrt'
    source = original.read_bytes()
    record = {'schema': 1, 'protocol_sha256': quality.PROTOCOL_SHA256,
              'scene_manifest_sha256': quality.sha(quality.ROOT / protocol['scene_manifest']),
              'backend': backend, 'scene': scene, 'spp': spp, 'resolution': protocol['resolution'],
              'sampler_seed': 0, 'material_seed': 0 if backend != 'baseline' else 'historical',
              'label': label, 'repetition': repetition, 'renderer': identity, 'status': 'running',
              'processes': [], 'variants': [], 'artifacts': {}}
    quality.write_json(receipt, record)
    try:
        for plane in ('clamped', 'raw') if backend == 'pbrt' else ('clamped',):
            output = (directory / f'{plane}.exr').resolve()
            variant = original.with_name(f'benchmark-{backend}-{scene}-{spp}-{name}-{plane}.pbrt')
            changed = quality.variant_source(source, scene=scene, spp=spp, seed=0,
                                             output=output, raw=plane == 'raw')
            variant.write_bytes(changed)
            transport = quality.replace_descriptor(changed, 'Film', b'Film')[0]
            record['variants'].append({'path': str(variant), 'sha256': quality.sha(variant),
                                       'original_sha256': quality.sha(original), 'plane': plane,
                                       'transport_sha256': hashlib.sha256(transport).hexdigest()})
            if backend == 'pbrt':
                command = [identity['binary'], '--nthreads', str(protocol['timing']['pbrt_threads']),
                           '--seed', '0', '--render-coord-sys', 'world', '--outfile', str(output),
                           str(variant)]
            else:
                command = [identity['binary'], '--render-pbrt', str(variant), '--resource-profile',
                           'large', '--output', str(output), '--raw-output', str(directory / 'raw.exr')]
                if backend == 'candidate':
                    command += ['--material-seed', '0']
            result = executor(command, directory / f'{plane}.log', directory / f'{plane}.time',
                              cwd=variant.parent, timeout=args.timeout)
            result['plane'] = plane
            result['phases'] = parse_phases(Path(result['log']).read_text(), backend)
            record['processes'].append(result)
            record['artifacts'][plane] = {'path': str(output), 'sha256': quality.sha(output),
                                         'scale': quality.required_scale(backend, scene, plane, protocol)}
        if backend != 'pbrt':
            output = directory / 'raw.exr'
            record['artifacts']['raw'] = {'path': str(output), 'sha256': quality.sha(output), 'scale': 1}
        elif len({v['transport_sha256'] for v in record['variants']}) != 1:
            raise ValueError('PBRT companion changed transport')
        record.update(status='complete',
                      total_raw_and_clamped_seconds=sum(p['wall_seconds'] for p in record['processes']),
                      peak_rss_bytes=max(p['peak_rss_bytes'] for p in record['processes']),
                      primary_clamped_seconds=record['processes'][0]['wall_seconds'],
                      raw_companion_seconds=record['processes'][1]['wall_seconds'] if backend == 'pbrt' else None,
                      raw_companion='separately rendered and timed; Film clamp removed, ISO retained'
                      if backend == 'pbrt' else 'written by the same render process')
    except Exception as error:
        record.update(status='failed', error=str(error))
        quality.write_json(receipt, record)
        raise
    quality.write_json(receipt, record)
    return record


def benchmark(args, protocol, staged, identities, *, execute_case=None):
    execute_case = execute_case or run_once
    summaries = []
    for backend, scene, spp in cases(protocol, args):
        runs = []
        for label, repetition in labels(protocol):
            # Deliberately blocking: no pool, background work, or parallel validation.
            runs.append(execute_case(args, protocol, staged, identities[backend], backend,
                                     scene, spp, label, repetition))
        measured = [r for r in runs if r['label'] == 'measured']
        seconds = [r['total_raw_and_clamped_seconds'] for r in measured]
        summaries.append({'backend': backend, 'scene': scene, 'spp': spp, 'runs': runs,
                          'mean_total_seconds': statistics.mean(seconds),
                          'stdev_total_seconds': statistics.stdev(seconds),
                          'minimum_total_seconds': min(seconds), 'maximum_total_seconds': max(seconds)})
    full_cases = {(b, s, n) for b in quality.BACKENDS for s, p in protocol['scenes'].items()
                  for n in p['budgets']}
    complete = {(s['backend'], s['scene'], s['spp']) for s in summaries} == full_cases
    result = {'schema': 1, 'protocol_sha256': quality.PROTOCOL_SHA256,
              'runner_sha256': quality.sha(Path(__file__)), 'serial': True,
              'warmups_per_case': 1, 'measured_repetitions': 3, 'complete_protocol': complete,
              'environment': {'system': platform.system(), 'release': platform.release(),
                              'machine': platform.machine(), 'hardware': getattr(args, 'hardware', None)},
              'wall_clock': 'perf_counter_ns around each blocking /usr/bin/time process',
              'cases': summaries}
    quality.write_json(args.output_dir / 'timing-summary.json', result)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, default=quality.ROOT / 'build/release/rypath')
    parser.add_argument('--baseline', type=Path, default=quality.ROOT / '.cache/pavilion-convergence/baseline/rypath')
    parser.add_argument('--pbrt', type=Path, default=quality.ROOT / '.cache/pbrt-reference/build/pbrt')
    parser.add_argument('--scene-cache', type=Path, default=quality.ROOT / '.cache/pbrt-pavilion')
    parser.add_argument('--output-dir', type=Path, default=quality.ROOT / '.cache/pavilion-convergence/timing')
    parser.add_argument('--scene', choices=('day', 'night'), action='append')
    parser.add_argument('--backend', choices=quality.BACKENDS, action='append')
    parser.add_argument('--spp', type=int, action='append')
    parser.add_argument('--timeout', type=int, default=7200)
    args = parser.parse_args(argv)
    if platform.system() != 'Darwin':
        raise ValueError('this timing runner requires macOS /usr/bin/time -l RSS byte units')
    protocol = quality.load_protocol()
    args.hardware = {name: subprocess.check_output(['sysctl', '-n', name], text=True).strip()
                     for name in ('machdep.cpu.brand_string', 'hw.model', 'hw.memsize')}
    list(cases(protocol, args))  # Reject invalid selections before any staging or rendering.
    args.output_dir = args.output_dir.resolve()
    manifest = quality.corpus.load_manifest(quality.ROOT / protocol['scene_manifest'])
    if manifest['revision'] != protocol['scene_revision']:
        raise ValueError('wrong scene revision')
    source = args.scene_cache.resolve()
    if not (source / 'barcelona-pavilion').is_dir():
        source /= manifest['revision']
    staged = quality.stage_closure(source, args.output_dir / 'staged' / manifest['revision'], manifest)
    identities = {b: quality.binary_identity(b, getattr(args, b), protocol)
                  for b in args.backend or quality.BACKENDS}
    result = benchmark(args, protocol, staged, identities)
    for backend, identity in identities.items():
        if quality.binary_identity(backend, getattr(args, backend), protocol) != identity:
            result.update(complete_protocol=False, identity_verification='failed: ' + backend)
            quality.write_json(args.output_dir / 'timing-summary.json', result)
            raise ValueError('renderer identity changed during timing: ' + backend)
    result['identity_verification'] = 'all renderer artifacts and source identities unchanged'
    quality.write_json(args.output_dir / 'timing-summary.json', result)
    print('Complete serial timing protocol retained' if result['complete_protocol']
          else 'Selected serial timings retained; complete protocol remains unestablished')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
