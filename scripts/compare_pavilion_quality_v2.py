#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Frozen two-arm,64-replicate Pavilion quality admission. Drafts cannot run."""
from __future__ import annotations
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import time

import compare_pavilion_quality as inherited

ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / 'tests/corpus/pavilion-quality-v2.json'
ARMS = ('candidate', 'reference')
SHA = re.compile(r'[0-9a-f]{64}')
INVARIANTS = ('scene_revision', 'scene_manifest', 'resolution', 'scenes', 'planes', 'regions',
              'region_bounds', 'luminance_coefficients', 'variance', 'tail', 'bootstrap',
              'primary_gate_count', 'maximum_ratio', 'epsilon', 'near_zero_gate', 'mean_gate')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    inherited.write_json(Path(path), value)


def utc(value):
    parsed = datetime.datetime.fromisoformat(value.replace('Z', '+00:00'))
    require(parsed.tzinfo is not None, 'UTC timestamp must have timezone')
    return parsed


def verify_bound(item):
    require(isinstance(item, dict) and set(item) >= {'path', 'sha256'}, 'missing bound receipt')
    require(SHA.fullmatch(item['sha256']) and sha(item['path']) == item['sha256'],
            'bound receipt changed')
    return json.loads(Path(item['path']).read_text())


def validate_protocol(value, *, allow_draft=False):
    old = inherited.load_protocol()
    require(value.get('schema') == 2 and value.get('protocol') == 'pavilion-quality-v2',
            'wrong v2 protocol')
    require(value.get('replicates') == 64 and value.get('arms') == list(ARMS),
            'exactly 64 real replicates in two arms required')
    for key in INVARIANTS:
        require(value.get(key) == old[key], 'changed inherited numerical policy: ' + key)
    require('baseline' not in value.get('seed_ids', {}) and 'baseline_revision' not in value,
            'historical baseline is not a v2 arm')
    policy = value.get('reference_policy', {})
    require(policy.get('kind') == 'modified-pbrt-diagnostic' and
            policy.get('stock_verified') is False, 'reference must be labeled modified PBRT')
    if value.get('status') == 'draft-unfrozen':
        require(allow_draft, 'draft protocol: root must freeze unused IDs/model before outcomes')
        return value
    require(value.get('status') == 'frozen-before-outcomes', 'unrecognized freeze state')
    freeze = value.get('freeze') or {}
    require(freeze.get('before_outcomes') is True, 'missing pre-outcome freeze assertion')
    utc(freeze['utc'])
    ledger = verify_bound(value.get('seed_usage_ledger'))
    require(ledger.get('inventory_complete') is True and ledger.get('bindings'),
            'observed seed inventory is incomplete')
    for path, digest in ledger['bindings'].items():
        require(SHA.fullmatch(digest) and sha(path) == digest, 'observed seed evidence changed')
    seeds = value.get('seed_ids', {})
    require(set(seeds) == set(ARMS), 'exactly two seed inventories required')
    require(set(value.get('artifacts') or {}) == set(ARMS), 'exactly two renderer artifacts required')
    for arm in ARMS:
        ids = seeds.get(arm)
        require(isinstance(ids, list) and len(ids) == 64 and
                all(type(x) is int and 0 <= x <= 2147483647 for x in ids),
                'missing 64 signed-domain IDs: ' + arm)
        require(len(set(ids)) == 64, 'duplicate seed IDs: ' + arm)
        observed = ledger.get('observed_ids', {}).get(arm)
        require(isinstance(observed, list) and all(type(x) is int for x in observed),
                'missing observed ID inventory: ' + arm)
        require(not set(ids).intersection(observed), 'observed seed reuse: ' + arm)
    require(not set(seeds['candidate']).intersection(seeds['reference']),
            'quality arms must use disjoint IDs')
    models = value.get('models') or {}
    require(isinstance(models.get('target_id'), str) and models['target_id'],
            'target model not frozen')
    review = verify_bound(models.get('review'))
    require(review.get('status') == 'approved-matched-target' and
            review.get('target_id') == models['target_id'], 'model equivalence review missing')
    for arm in ARMS:
        require(models.get(arm, {}).get('target_id') == models['target_id'] and
                SHA.fullmatch(models[arm].get('model_sha256', '')) and
                review.get('arm_model_sha256', {}).get(arm) == models[arm]['model_sha256'],
                'wrong or missing arm model identity')
        artifact = (value.get('artifacts') or {}).get(arm, {})
        require(artifact.get('kind') == ('rypath' if arm == 'candidate'
                                       else 'modified-pbrt-diagnostic') and
                SHA.fullmatch(artifact.get('binary_sha256', '')) and artifact.get('binary'),
                'missing frozen renderer artifact: ' + arm)
        require(artifact.get('stock_verified') is False if arm == 'reference' else True,
                'modified reference cannot claim stock verification')
        require(SHA.fullmatch(artifact.get('source_manifest_sha256', '')) and
                artifact.get('source_manifest'), 'missing source provenance')
    engine = value.get('metrics_engine') or {}
    require(engine.get('kind') == 'pavilion-quality-v2-two-arm' and engine.get('binary') and
            SHA.fullmatch(engine.get('sha256', '')) and
            SHA.fullmatch(engine.get('source_sha256', '')) and
            SHA.fullmatch(engine.get('numerical_core_sha256', '')),
            'two-arm metrics engine provenance not frozen')
    contracts = value.get('contracts') or {}
    require(set(contracts) == set(ARMS), 'exactly two contract identities required')
    require(isinstance(contracts.get('candidate'), list) and len(contracts['candidate']) == 5 and
            all(type(x) is int and x >= 0 for x in contracts['candidate']) and
            contracts.get('reference') == 'modified-pbrt-v4', 'contracts not frozen')
    return value


def load_protocol(path, expected_sha, *, allow_draft=False):
    require(SHA.fullmatch(expected_sha or '') and sha(path) == expected_sha,
            'protocol SHA mismatch')
    return validate_protocol(json.loads(Path(path).read_text()), allow_draft=allow_draft)


def artifact_identity(protocol, arm):
    item = protocol['artifacts'][arm]
    require(sha(item['binary']) == item['binary_sha256'], 'renderer bytes changed')
    require(sha(item['source_manifest']) == item['source_manifest_sha256'],
            'renderer source provenance changed')
    if arm == 'candidate':
        # The CLI canonicalizes its executable before loading this sibling;
        # it has no protocol-supplied library override.
        loaded_library = Path(item['binary']).resolve().with_name('rypath.metallib')
        require(item.get('metallib') and
                loaded_library == Path(item['metallib']).resolve(),
                'candidate metallib is not the resolved executable sibling')
        require(SHA.fullmatch(item.get('metallib_sha256', '')) and
                sha(loaded_library) == item['metallib_sha256'], 'candidate metallib changed')
    require(os.access(item['binary'], os.X_OK), 'renderer not executable')
    return dict(item)


def required_scale(arm, scene, plane, protocol):
    require(arm in ARMS and scene in protocol['scenes'] and plane in protocol['planes'],
            'wrong endpoint identity')
    return 100 / protocol['scenes'][scene]['iso'] if arm == 'reference' and plane == 'raw' else 1


def read_image(path):
    # Same canonical exposure operation as inherited native read_image: binary64 multiply,
    # one FLOAT rounding. This reader also enforces the complete original raster window.
    import OpenEXR
    import numpy as np
    with OpenEXR.File(str(path), separate_channels=True) as image:
        header, channels = image.header(), image.channels()
        expected = ([0, 0], [319, 169])
        for key in ('dataWindow', 'displayWindow'):
            require(tuple(list(x) for x in header[key]) == expected, 'wrong image window')
        require(set(channels) == set('RGB'), 'wrong RGB channels')
        require(all(c.type() == OpenEXR.FLOAT and c.pixels.dtype == np.float32 and
                    c.pixels.shape == (170, 320) for c in channels.values()),
                'quality requires complete 320x170 FLOAT RGB')
        pixels = np.stack([channels[c].pixels for c in 'RGB'], axis=-1)
        require(np.isfinite(pixels).all(), 'nonfinite output image')
    return {'resolution': [320, 170], 'data_window': [[0, 0], [319, 169]],
            'display_window': [[0, 0], [319, 169]], 'channels': ['R', 'G', 'B'],
            'storage': 'FLOAT'}


def checkpoint_contract(path, expected, settings=None):
    data = Path(path).read_bytes()
    header = struct.Struct('<8s5I2Q')
    require(len(data) >= header.size + 32, 'checkpoint truncated')
    magic, *fields = header.unpack_from(data)
    require(magic == b'RYPCHK\r\n' and fields[:5] == expected, 'mixed checkpoint contracts')
    require(len(data) == header.size + fields[5] + fields[6] + 32 and
            hashlib.sha256(data[:-32]).digest() == data[-32:], 'checkpoint length/checksum')
    if settings is not None:
        metadata = struct.Struct('<IIQQQIIiIdBBdIfff32sI')
        require(expected[0] == 3 and fields[5] >= metadata.size,
                'unsupported candidate checkpoint format')
        values = metadata.unpack_from(data, header.size)
        width, height, target, _batch, seed, _sampler, _randomization, material_seed = values[:8]
        require((width, height, target, seed, material_seed) ==
                (320, 170, settings['spp'], settings['sampler_seed'], settings['material_seed']),
                'checkpoint settings/seed mismatch')
        require(fields[6] == 320 * 170 * 216, 'incomplete checkpoint Film')
        payload = header.size + fields[5]
        require(all(struct.unpack_from('<Q', data, payload + pixel * 216 + 48)[0] == target
                    for pixel in range(320 * 170)), 'incomplete checkpoint sample counts')
    return expected


def record_path(root, arm, scene, spp, seed):
    return Path(root) / 'runs' / arm / scene / str(spp) / f'seed-{seed:010d}' / 'record.json'


def validate_record(record, protocol, protocol_sha, *, image_reader=read_image):
    require(record.get('kind') == 'quality-v2-replicate' and record.get('status') == 'complete' and
            record.get('protocol_sha256') == protocol_sha, 'incomplete or wrong protocol record')
    arm, scene, seed = record.get('arm'), record.get('scene'), record.get('sampler_seed')
    require(arm in ARMS and scene in protocol['scenes'] and
            seed in protocol['seed_ids'][arm] and record.get('material_seed') == seed and
            record.get('spp') == protocol['scenes'][scene]['primary_spp'], 'wrong endpoint/seed')
    require(record.get('model') == protocol['models'][arm] and
            record.get('target_id') == protocol['models']['target_id'], 'wrong model identity')
    require(record.get('renderer') == protocol['artifacts'][arm], 'mixed renderer identities')
    require(record.get('contract') == protocol['contracts'][arm], 'mixed contracts')
    require(utc(record['started_utc']) >= utc(protocol['freeze']['utc']),
            'measurement predates seed/model freeze')
    require(set(record.get('artifacts', {})) == {'raw', 'clamped'}, 'partial endpoint planes')
    for plane, item in record['artifacts'].items():
        scale = item.get('scale')
        require(type(scale) in (int, float) and math.isfinite(scale) and
                scale == required_scale(arm, scene, plane, protocol), 'wrong retained scale')
        require(sha(item['path']) == item['sha256'], 'image bytes changed')
        canonical = {'resolution': [320, 170], 'data_window': [[0, 0], [319, 169]],
                     'display_window': [[0, 0], [319, 169]], 'channels': ['R', 'G', 'B'],
                     'storage': 'FLOAT'}
        require(item.get('image') == canonical and image_reader(item['path']) == canonical,
                'wrong image metadata/window')
    original = record.get('original_scene', {})
    require(original.get('path') and sha(original['path']) == original.get('sha256') and
            Path(original['path']).name == f'pavilion-{scene}.pbrt', 'wrong original scene source')
    original_bytes = Path(original['path']).read_bytes()
    variants = record.get('variants', [])
    require(len(variants) == (2 if arm == 'reference' else 1), 'missing source variants')
    require({v.get('plane') for v in variants} ==
            ({'raw', 'clamped'} if arm == 'reference' else {'clamped'}),
            'wrong variant plane inventory')
    for item in variants:
        require(sha(item['path']) == item['sha256'], 'source variant changed')
        expected_bytes = inherited.variant_source(original_bytes, scene=scene,
                             spp=record['spp'], seed=seed,
                             output=Path(record['artifacts'][item['plane']]['path']),
                             raw=item['plane'] == 'raw')
        require(Path(item['path']).read_bytes() == expected_bytes,
                'variant settings/seed/world differ from frozen source')
        transport = inherited.replace_descriptor(expected_bytes, 'Film', b'Film')[0]
        require(item.get('transport_sha256') == hashlib.sha256(transport).hexdigest(),
                'transport digest differs from actual source')
    require(len({v['transport_sha256'] for v in variants}) == 1,
            'raw companion transport changed')
    if arm == 'candidate':
        item = record.get('checkpoint', {})
        require(item.get('path') and sha(item['path']) == item.get('sha256'),
                'missing authenticated checkpoint')
        checkpoint_contract(item['path'], protocol['contracts'][arm], record)
    processes = record.get('processes', [])
    require(len(processes) == (2 if arm == 'reference' else 1) and
            all(p.get('exit_code') == 0 and p.get('status') == 'exited' for p in processes),
            'incomplete renderer processes')
    for process, variant in zip(processes, variants):
        output = record['artifacts'][variant['plane']]['path']
        require(process.get('command') == renderer_command(protocol, arm, seed,
                         variant['path'], output, Path(record['artifacts']['raw']['path']).parent),
                'actual renderer command/seed/model differs')
        require(sha(process['log']) == process['log_sha256'], 'renderer log changed')
        reject_log(process['log'])
    return record


def collect_records(root, protocol, protocol_sha, *, image_reader=read_image):
    records = []
    expected_paths = set()
    for scene, settings in protocol['scenes'].items():
        for arm in ARMS:
            for seed in protocol['seed_ids'][arm]:
                path = record_path(root, arm, scene, settings['primary_spp'], seed)
                expected_paths.add(path.resolve())
                require(path.is_file(), 'missing required replicate: ' + str(path))
                record = validate_record(json.loads(path.read_text()), protocol, protocol_sha,
                                         image_reader=image_reader)
                require((record['arm'], record['scene'], record['sampler_seed']) ==
                        (arm, scene, seed), 'record in wrong endpoint slot')
                records.append(record)
    actual = {p.resolve() for p in (Path(root) / 'runs').rglob('record.json')}
    require(actual == expected_paths and len(records) == 256, 'extra/partial endpoint inventory')
    return records


def close(a, b):
    # Arithmetic cross-check only: an absolute1e-12 tolerance would incorrectly
    # bless altered near-zero epsilon bounds. Keep the scale of actual operands.
    return abs(a - b) <= 64 * max(math.ulp(a), math.ulp(b))


def validate_analysis_result(value, protocol):
    require(value.get('engine') == 'pavilion-quality-v2-two-arm' and
            value.get('replicates') == 64 and value.get('backend_count') == 2 and
            value.get('resolution') == [320, 170], 'wrong native v2 endpoint contract')
    require(value.get('bootstrap_draws') == 20000 and
            value.get('bootstrap_seed') == protocol['bootstrap']['seed'] and
            value.get('bootstrap_rng') == 'mt19937-per-draw-seed-seq-rejection',
            'wrong bootstrap method/budget')
    margin = value.get('simultaneous_log_margin')
    require(type(margin) in (int, float) and math.isfinite(margin) and margin >= 0,
            'inconclusive simultaneous bound')
    expected = {(f'{s}/{p}', r, m) for s in protocol['scenes'] for p in protocol['planes']
                for r in protocol['regions'] for m in ('variance', 'tail')}
    gates = value.get('gates', [])
    keys = [(g.get('dataset'), g.get('region'), g.get('metric')) for g in gates]
    require(len(keys) == len(set(keys)) == 48 and set(keys) == expected and
            value.get('gate_count') == 48, 'duplicate/missing endpoint gate family')
    passed = True
    for gate in gates:
        require('baseline' not in gate, 'fabricated historical baseline field')
        c, r, epsilon, upper = (gate.get(k) for k in ('candidate', 'reference', 'epsilon', 'upper'))
        require(all(type(x) in (int, float) and math.isfinite(x) for x in (c, r, epsilon)) and
                c >= 0 and r >= 0 and epsilon > 0, 'invalid metric energy/epsilon')
        absolute = r <= epsilon
        score = math.log(max(c, sys.float_info.min)) - math.log(epsilon if absolute else r)
        log_upper = score + margin
        require(type(gate.get('absolute')) is bool and gate['absolute'] == absolute and
                math.isfinite(gate.get('log_upper', math.nan)) and
                close(gate['log_upper'], log_upper), 'wrong epsilon/log bound branch')
        try:
            computed_upper = 0 if c == 0 else math.exp(log_upper + (math.log(epsilon) if absolute else 0))
        except OverflowError:
            computed_upper = None
        require((upper is None and computed_upper is None) or
                (type(upper) in (int, float) and math.isfinite(upper) and upper >= 0 and
                 computed_upper is not None and close(upper, computed_upper)),
                'upper bound inconsistent with simultaneous margin')
        ok = upper is not None and upper <= 1.25 * (epsilon if absolute else 1)
        require(type(gate.get('pass')) is bool and gate['pass'] == ok, 'wrong metric verdict')
        passed &= ok
    means = value.get('mean_gates', [])
    expected_means = {(f'{s}/raw', r, c) for s in protocol['scenes']
                      for r in protocol['regions'] for c in range(3)}
    keys = [(g.get('dataset'), g.get('region'), g.get('channel')) for g in means]
    require(len(keys) == len(set(keys)) == 36 and set(keys) == expected_means,
            'duplicate/missing raw RGB mean family')
    for gate in means:
        fields = [gate.get(k) for k in ('candidate', 'reference', 'difference', 'combined_se', 'threshold')]
        require(all(type(x) in (int, float) and math.isfinite(x) for x in fields) and
                gate['combined_se'] >= 0, 'nonfinite mean gate')
        difference = gate['candidate'] - gate['reference']
        threshold = 5 * gate['combined_se'] + 2e-5 * max(1, abs(gate['reference']))
        require(close(gate['difference'], difference) and close(gate['threshold'], threshold),
                'changed raw mean rule')
        ok = abs(difference) <= threshold
        require(type(gate.get('pass')) is bool and gate['pass'] == ok, 'wrong raw mean verdict')
        passed &= ok
    require(value.get('complete') is True and type(value.get('accepted')) is bool and
            value['accepted'] == passed, 'incomplete/inconsistent aggregate acceptance')
    return passed


def reject_log(path):
    text = Path(path).read_text(errors='replace')
    require(not re.search(r'\b(?:FATAL|ERROR|nonfinite|NaN)\b|Setting to black|'
                          r'Not-a-number radiance|Infinite radiance', text, re.I),
            'invalid radiance/renderer diagnostic retained')


def process_group_members(pgid):
    text = subprocess.check_output(['ps', '-axo', 'pid=,pgid='], text=True)
    return [int(words[0]) for line in text.splitlines()
            if len(words := line.split()) == 2 and int(words[1]) == pgid]


def owned_process(command, log, *, cwd, timeout, row, persist):
    """Initial receipt, owned PGID, bounded wait; no failure can imply a clean idle."""
    row.update(command=list(map(str, command)), cwd=str(cwd), log=str(log),
               status='launching', pid=None, exit_code=None, timeout_seconds=timeout)
    persist()
    child = None
    def interrupted(signum, _frame):
        raise KeyboardInterrupt('interrupted signal ' + str(signum))
    prior = {sig: signal.signal(sig, interrupted)
             for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    started = time.monotonic()
    try:
        with Path(log).open('xb') as stream:
            child = subprocess.Popen(row['command'], cwd=cwd, stdout=stream,
                                     stderr=subprocess.STDOUT, start_new_session=True)
            row.update(pid=child.pid, pgid=child.pid, status='running')
            persist()
            row['exit_code'] = child.wait(timeout=timeout)
        require(not process_group_members(child.pid), 'owned descendants remain after exit')
        row.update(status='exited', elapsed_seconds=time.monotonic() - started,
                   log_sha256=sha(log))
        persist()
        require(row['exit_code'] == 0, 'renderer/helper failed; outputs preserved')
    except BaseException:
        if child is None:
            row['requires_root_review'] = True
        else:
            try:
                if process_group_members(child.pid):
                    os.killpg(child.pid, signal.SIGKILL)
                row['exit_code'] = child.wait(timeout=10)
                require(not process_group_members(child.pid), 'cleanup left descendants')
            except BaseException as cleanup:
                row.update(cleanup_error=repr(cleanup), requires_root_review=True)
        row.update(status='failed-preserved', elapsed_seconds=time.monotonic() - started)
        if Path(log).exists():
            row['log_sha256'] = sha(log)
        persist()
        raise
    finally:
        for sig, handler in prior.items():
            signal.signal(sig, handler)


def idle():
    text = subprocess.check_output(['ps', '-axo', 'pid=,comm='], text=True)
    names = {'rypath', 'pbrt', 'host', 'control', 'probe', 'replay_day_owner', 'ctest',
             'ninja', 'cmake', 'metal', 'metallib', 'clang', 'clang++', 'c++', 'cc',
             'gcc', 'g++', 'xctrace', 'rypath_pavilion_quality_metrics',
             'rypath_pavilion_quality_metrics_v2'}
    active = []
    for line in text.splitlines():
        words = line.strip().split(maxsplit=1)
        if len(words) != 2 or int(words[0]) == os.getpid():
            continue
        name = Path(words[1]).name
        if name in names or name.startswith(('probe-', 'probe_', 'rypath_')):
            active.append(line)
    require(not active, 'renderer/compiler/profiler active: ' + repr(active))


def quality_inventory(protocol):
    return [(arm, scene, settings['primary_spp'], seed)
            for scene, settings in protocol['scenes'].items()
            for index in range(64) for arm in ARMS
            for seed in [protocol['seed_ids'][arm][index]]]


def renderer_command(protocol, arm, seed, variant, output, directory):
    binary = protocol['artifacts'][arm]['binary']
    if arm == 'reference':
        return [binary, '--seed', str(seed), '--nthreads', '16', '--render-coord-sys',
                'world', '--outfile', str(output), str(variant)]
    return [binary, '--render-pbrt', str(variant), '--resource-profile', 'large',
            '--output', str(output), '--raw-output', str(directory / 'raw.exr'),
            '--material-seed', str(seed), '--batch-size', '8',
            '--checkpoint', str(directory / 'complete.rycp'), '--checkpoint-interval', '60']


def render_one(root, staged, protocol, protocol_sha, arm, scene, spp, seed, timeout):
    path = record_path(root, arm, scene, spp, seed)
    require(not path.parent.exists(), 'fresh output directory required; no retry/resume')
    path.parent.mkdir(parents=True)
    identity = artifact_identity(protocol, arm)
    record = {'kind': 'quality-v2-replicate', 'status': 'running',
              'protocol_sha256': protocol_sha, 'arm': arm, 'scene': scene, 'spp': spp,
              'sampler_seed': seed, 'material_seed': seed,
              'model': protocol['models'][arm], 'target_id': protocol['models']['target_id'],
              'renderer': identity, 'contract': protocol['contracts'][arm],
              'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'artifacts': {}, 'variants': [], 'processes': []}
    persist = lambda: write_json(path, record)
    persist()
    try:
        source = staged / 'barcelona-pavilion' / f'pavilion-{scene}.pbrt'
        original = source.read_bytes()
        record['original_scene'] = {'path': str(source), 'sha256': sha(source)}
        for plane in ('clamped', 'raw') if arm == 'reference' else ('clamped',):
            output = path.parent / (plane + '.exr')
            # Keep the entry point beside its unchanged dependencies, as in v1.
            variant = source.with_name(f'quality-v2-{arm}-{scene}-{seed}-{plane}.pbrt')
            require(not variant.exists(), 'source variant already exists')
            changed = inherited.variant_source(original, scene=scene, spp=spp, seed=seed,
                                                output=output, raw=plane == 'raw')
            variant.write_bytes(changed)
            transport = inherited.replace_descriptor(changed, 'Film', b'Film')[0]
            record['variants'].append({'path': str(variant), 'sha256': sha(variant),
                                       'plane': plane,
                                       'transport_sha256': hashlib.sha256(transport).hexdigest()})
            command = renderer_command(protocol, arm, seed, variant, output, path.parent)
            row = {'plane': plane}
            record['processes'].append(row)
            persist()
            idle()
            owned_process(command, path.parent / (plane + '.log'), cwd=variant.parent,
                          timeout=timeout, row=row, persist=persist)
            idle()
            reject_log(row['log'])
        for plane in ('raw', 'clamped'):
            image = path.parent / (plane + '.exr')
            record['artifacts'][plane] = {'path': str(image), 'sha256': sha(image),
                                         'scale': required_scale(arm, scene, plane, protocol),
                                         'image': read_image(image)}
        if arm == 'candidate':
            checkpoint = path.parent / 'complete.rycp'
            checkpoint_contract(checkpoint, protocol['contracts']['candidate'], record)
            record['checkpoint'] = {'path': str(checkpoint), 'sha256': sha(checkpoint)}
        artifact_identity(protocol, arm)
        record['status'] = 'complete'
        validate_record(record, protocol, protocol_sha)
    except BaseException as error:
        record.update(status='failed-preserved', error=repr(error),
                      requires_root_review=any(x.get('requires_root_review')
                                               for x in record['processes']))
        raise
    finally:
        record['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        persist()


def analyze(root, protocol, protocol_sha, metrics, threads, timeout):
    root = Path(root)
    engine = protocol['metrics_engine']
    require(metrics.resolve() == Path(engine['binary']).resolve() and
            metrics.is_file() and os.access(metrics, os.X_OK) and sha(metrics) == engine['sha256'],
            'missing or changed frozen v2 metrics helper')
    require(not any((root / n).exists() for n in ('quality-metrics-v2.json',
                                                 'metrics-input-v2.txt', 'analysis-execution.json')),
            'fresh analysis required; retain failed results')
    closure = inherited.corpus.load_manifest(ROOT / protocol['scene_manifest'])
    inherited.verify_closure(root / 'staged' / closure['revision'], closure)
    records = collect_records(root, protocol, protocol_sha)
    indexed = {(r['arm'], r['scene'], r['sampler_seed']): r for r in records}
    lines = [f'PAVILION_METRICS_2 320 170 64 20000 {protocol["bootstrap"]["seed"]} 4 6 {threads}']
    lines.extend(name + ' ' + ' '.join(map(str, bounds))
                 for name, bounds in protocol['regions'].items())
    for scene in protocol['scenes']:
        for plane in protocol['planes']:
            lines.append(scene + '/' + plane)
            for arm in ARMS:
                for seed in protocol['seed_ids'][arm]:
                    item = indexed[arm, scene, seed]['artifacts'][plane]
                    lines.append(json.dumps(item['path']) + ' ' + str(item['scale']))
    manifest = root / 'metrics-input-v2.txt'
    manifest.write_text('\n'.join(lines) + '\n')
    result_path = root / 'quality-metrics-v2.json'
    receipt = {'status': 'running', 'protocol_sha256': protocol_sha,
               'metrics_binary_sha256': sha(metrics), 'manifest_sha256': sha(manifest),
               'records': [{'path': str(record_path(root, r['arm'], r['scene'], r['spp'],
                                                    r['sampler_seed'])),
                            'sha256': sha(record_path(root, r['arm'], r['scene'], r['spp'],
                                                      r['sampler_seed']))} for r in records],
               'process': {}}
    persist = lambda: write_json(root / 'analysis-execution.json', receipt)
    persist()
    try:
        idle()
        owned_process([str(metrics), 'analyze', str(manifest), str(result_path)],
                      root / 'analysis-v2.log', cwd=root, timeout=timeout,
                      row=receipt['process'], persist=persist)
        idle()
        value = json.loads(result_path.read_text())
        accepted = validate_analysis_result(value, protocol)
        # Verify inputs again; outcome cannot bless changed or incomplete inventory.
        collect_records(root, protocol, protocol_sha)
        receipt.update(status='complete-accepted' if accepted else 'complete-failed-gates',
                       accepted=accepted, result_sha256=sha(result_path))
        return accepted
    except BaseException as error:
        receipt.update(status='failed-or-inconclusive-preserved', accepted=False, error=repr(error))
        raise
    finally:
        persist()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('plan', 'render', 'analyze'))
    parser.add_argument('--protocol', type=Path, default=PROTOCOL)
    parser.add_argument('--protocol-sha', required=True)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--scene-cache', type=Path, default=ROOT / '.cache/pbrt-pavilion')
    parser.add_argument('--metrics', type=Path, default=ROOT / 'build/release/rypath_pavilion_quality_metrics_v2')
    parser.add_argument('--root-go', action='store_true')
    parser.add_argument('--timeout', type=int, default=7200)
    parser.add_argument('--analysis-timeout', type=int, default=43200)
    parser.add_argument('--threads', type=int, default=16)
    args = parser.parse_args(argv)
    protocol = load_protocol(args.protocol, args.protocol_sha, allow_draft=args.mode == 'plan')
    if args.mode == 'plan':
        print(json.dumps({'status': protocol['status'], 'replicates': 64, 'arms': ARMS,
                          'executable': protocol['status'] == 'frozen-before-outcomes',
                          'required_primary_records': 256, 'quality_gate_count': 48,
                          'raw_mean_gate_count': 36}, indent=2))
        return 0
    require(args.root_go and args.output_dir is not None, 'root scheduling GO and fresh output required')
    require(0 < args.timeout <= 86400 and 0 < args.analysis_timeout <= 43200 and
            1 <= args.threads <= 64, 'invalid operational budget')
    require(not any(k.startswith(('MTL_DEBUG', 'MTL_GPU_VALIDATION', 'MTL_SHADER_VALIDATION')) or
                    k.startswith(('RYPATH_NATIVE_HINT', 'RYPATH_DAY_MAP')) for k in os.environ),
            'uncontrolled validation/experimental environment')
    root = args.output_dir.resolve()
    for arm in ARMS:
        artifact_identity(protocol, arm)
    if args.mode == 'analyze':
        return 0 if analyze(root, protocol, args.protocol_sha, args.metrics.resolve(),
                            args.threads, args.analysis_timeout) else 1
    require(not root.exists(), 'fresh quality root required; no retry/resume/seed extension')
    root.mkdir(parents=True)
    manifest = inherited.corpus.load_manifest(ROOT / protocol['scene_manifest'])
    require(manifest['revision'] == protocol['scene_revision'], 'scene revision mismatch')
    source = args.scene_cache.resolve()
    if not (source / 'barcelona-pavilion').is_dir():
        source /= manifest['revision']
    staged = inherited.stage_closure(source, root / 'staged' / manifest['revision'], manifest)
    study = {'status': 'running', 'protocol_sha256': args.protocol_sha,
             'fixed_inventory': quality_inventory(protocol), 'complete': False}
    write_json(root / 'study.json', study)
    try:
        for arm, scene, spp, seed in study['fixed_inventory']:
            render_one(root, staged, protocol, args.protocol_sha, arm, scene, spp, seed,
                       args.timeout)
        collect_records(root, protocol, args.protocol_sha)
        study.update(status='complete-fixed-two-arm-inventory', complete=True)
    except BaseException as error:
        study.update(status='failed-preserved', complete=False, error=repr(error))
        raise
    finally:
        write_json(root / 'study.json', study)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
