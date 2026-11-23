#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run and analyze the frozen Pavilion quality protocol without dropping replicates.

Incremental selections retain receipts but cannot establish final acceptance.
All bootstrap pixels, leave-one-out ranks and simultaneous bounds run natively.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

import fetch_pbrt_pavilion as corpus
from compare_mesh_shapes import verify_reference
from compare_packet_transport import replace_descriptor

ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / 'tests/corpus/pavilion-quality.json'
PROTOCOL_SHA256 = '6657ac1a3a8ef296f6fbcf7ef58190e35374e210983db03514a06d9e28cdd99d'
BACKENDS = ('candidate', 'pbrt', 'baseline')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')
    os.replace(temporary, path)


def load_protocol(path=PROTOCOL):
    if sha(path) != PROTOCOL_SHA256:
        raise ValueError('frozen Pavilion protocol hash mismatch')
    value = json.loads(path.read_text())
    if (value['bootstrap']['draws'] != 20000 or value['replicates'] != 32 or
            value['primary_gate_count'] != 48 or value['maximum_ratio'] != 1.25):
        raise ValueError('invalid frozen protocol')
    return value


def renderer_source_identity(root=ROOT):
    names = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-co',
                                     '--exclude-standard'], text=True).splitlines()
    selected = sorted(set(name for name in names if name.startswith(('src/', 'include/',
                      'shaders/', 'cmake/')) or name == 'CMakeLists.txt'))
    files = {name: sha(root / name) for name in selected if (root / name).is_file()}
    return {'revision': subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'],
                                                text=True).strip(),
            'source_sha256': hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest(),
            'files': files}


def stage_closure(source, output, manifest):
    """Preserve both barcelona-pavilion/ and its sibling landscape/ exactly."""
    if len(manifest['files']) != 159:
        raise ValueError('expected the pinned 159-asset closure')
    for item in manifest['files']:
        original = corpus.safe_path(source, item['path'])
        if not corpus.verified(original, item):
            raise ValueError('dependency hash mismatch: ' + item['path'])
        target = corpus.safe_path(output, item['path'])
        if not corpus.verified(target, item):
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original, target)
        if not corpus.verified(target, item):
            raise ValueError('staged dependency hash mismatch: ' + item['path'])
    return output


def verify_closure(root, manifest):
    if len(manifest['files']) != 159:
        raise ValueError('expected the pinned 159-asset closure')
    for item in manifest['files']:
        if not corpus.verified(corpus.safe_path(root, item['path']), item):
            raise ValueError('dependency hash mismatch: ' + item['path'])


def parameter(block, kind, name, value, remove=False):
    pattern = re.compile(rb'"' + kind.encode() + b' ' + name.encode() +
                         rb'"\s*(?:\[[^\]]*\]|"(?:\\.|[^"\\])*"|[^\s#]+)')
    matches = list(pattern.finditer(block))
    if len(matches) > 1:
        raise ValueError('duplicate descriptor parameter: ' + name)
    replacement = b'' if remove else f'"{kind} {name}" [{value}]'.encode()
    if matches:
        return pattern.sub(lambda _: replacement, block, count=1)
    return block if remove else block.rstrip() + b'\n    ' + replacement + b'\n'


def variant_source(source, *, scene, spp, seed, output, raw=False):
    """Modify Film and sampler budget/seed only; camera and world remain exact."""
    original_world = source.split(b'WorldBegin', 1)[1]
    _, film = replace_descriptor(source, 'Film', b'Film "rgb"')
    changed = film
    for kind, name, value in [('integer', 'xresolution', '320'),
                              ('integer', 'yresolution', '170'),
                              ('string', 'filename', json.dumps(str(output))),
                              ('bool', 'savefp16', 'false')]:
        changed = parameter(changed, kind, name, value)
    if raw:
        changed = parameter(changed, 'float', 'maxcomponentvalue', '', remove=True)
    source, _ = replace_descriptor(source, 'Film', changed.rstrip())
    _, sampler = replace_descriptor(source, 'Sampler', b'Sampler "halton"')
    changed = parameter(sampler, 'integer', 'pixelsamples', str(spp))
    changed = parameter(changed, 'integer', 'seed', str(seed))
    source, _ = replace_descriptor(source, 'Sampler', changed.rstrip())
    if source.split(b'WorldBegin', 1)[1] != original_world:
        raise ValueError('variant changed world data')
    return source


def candidate_source_identity(binary, library):
    """Bind copied executables to their frozen build receipt, not today's checkout."""
    path = binary.with_name('artifacts.json')
    if not path.exists():
        return renderer_source_identity()
    receipt = json.loads(path.read_text())
    if not isinstance(receipt, dict) or not isinstance(receipt.get('files'), dict):
        raise ValueError('invalid frozen candidate receipt')
    for artifact in (binary, library):
        retained = receipt.get('files', {}).get(str(artifact), {})
        if (not isinstance(retained, dict) or retained.get('sha256') != sha(artifact) or
                ('bytes' in retained and retained['bytes'] != artifact.stat().st_size)):
            raise ValueError('frozen candidate artifact hash or size mismatch')
    source = receipt.get('source', {})
    if not isinstance(source, dict) or not isinstance(source.get('revision'), str):
        raise ValueError('invalid frozen candidate source identity')
    files = source.get('files')
    if (not re.fullmatch(r'[0-9a-f]{40}', source.get('revision', '')) or
            not isinstance(files, dict) or not files):
        raise ValueError('invalid frozen candidate source identity')
    for name, digest in files.items():
        relative = Path(name)
        if (relative.is_absolute() or '..' in relative.parts or str(relative) != name or
                not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest)):
            raise ValueError('invalid frozen candidate source file')
    canonical = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    if source.get('source_sha256') != canonical:
        raise ValueError('frozen candidate source digest mismatch')
    return {'revision': source['revision'], 'source_sha256': canonical, 'files': files}


def binary_identity(backend, binary, protocol):
    binary = binary.resolve()
    identity = {'binary': str(binary), 'binary_sha256': sha(binary)}
    if backend == 'pbrt':
        identity['revision'] = verify_reference(binary)
    else:
        library = binary.with_name('rypath.metallib')
        identity.update(metallib=str(library), metallib_sha256=sha(library))
        if backend == 'candidate':
            identity.update(candidate_source_identity(binary, library))
        else:
            receipt = json.loads(binary.with_name('artifacts.json').read_text())
            if receipt['base_revision'] != protocol['baseline_revision']:
                raise ValueError('wrong frozen baseline revision')
            for path in (binary, library):
                if receipt['files'].get(str(path), {}).get('sha256') != sha(path):
                    raise ValueError('frozen baseline artifact hash mismatch')
            identity['revision'] = receipt['base_revision']
    return identity


def record_path(output, backend, scene, spp, seed):
    return output / 'runs' / backend / scene / str(spp) / f'seed-{seed:03}' / 'record.json'


def required_scale(backend, scene, plane, protocol):
    if backend not in BACKENDS or scene not in protocol['scenes'] or plane not in protocol['planes']:
        raise ValueError('invalid retained exposure identity')
    if backend == 'pbrt' and plane == 'raw':
        return 100 / protocol['scenes'][scene]['iso']
    return 1


def validate_record(path, expected=None):
    record = json.loads(path.read_text())
    if record.get('protocol_sha256') != PROTOCOL_SHA256 or record.get('status') != 'complete':
        raise ValueError('incomplete or wrong-protocol retained record')
    if expected:
        for key, value in expected.items():
            if record.get(key) != value:
                raise ValueError('reuse identity mismatch: ' + key)
    if set(record['artifacts']) != {'raw', 'clamped'}:
        raise ValueError('missing retained planes')
    protocol = load_protocol()
    for name, item in record['artifacts'].items():
        if not isinstance(item.get('scale'), (int, float)) or not math.isfinite(item['scale']) or item['scale'] <= 0:
            raise ValueError('nonfinite or invalid retained exposure')
        if item['scale'] != required_scale(record.get('backend'), record.get('scene'), name, protocol):
            raise ValueError('retained exposure does not match frozen protocol: ' + name)
        artifact = Path(item['path'])
        if not artifact.is_file() or sha(artifact) != item['sha256']:
            raise ValueError('retained artifact hash mismatch: ' + name)
        if 'pfm' in item and sha(item['pfm']) != item.get('pfm_sha256'):
            raise ValueError('retained PFM hash mismatch: ' + name)
    variants = record.get('variants', [])
    if not variants:
        raise ValueError('missing retained source receipt')
    for item in variants:
        if sha(item['path']) != item['sha256']:
            raise ValueError('retained source hash mismatch')
    if record.get('backend') == 'pbrt' and len({v['transport_sha256'] for v in variants}) != 1:
        raise ValueError('PBRT raw companion transport mismatch')
    return record


def extract_record(record, helper):
    for plane in ('raw', 'clamped'):
        item = record['artifacts'][plane]
        pfm = Path(item['path']).with_suffix('.pfm')
        subprocess.run([str(helper), 'extract', item['path'], str(pfm), str(item['scale'])],
                       check=True, timeout=120)
        item.update(pfm=str(pfm), pfm_sha256=sha(pfm))
    return record


def render_one(args, protocol, manifest, staged, backend, scene, spp, seed, identity):
    path = record_path(args.output_dir, backend, scene, spp, seed)
    expected = {'protocol_sha256': PROTOCOL_SHA256, 'backend': backend, 'scene': scene,
                'spp': spp, 'sampler_seed': seed,
                'material_seed': seed if backend != 'baseline' else 'historical',
                'renderer': identity, 'scene_manifest_sha256': sha(ROOT / protocol['scene_manifest'])}
    if args.reuse and path.exists():
        validate_record(path, expected)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    source = staged / 'barcelona-pavilion' / f'pavilion-{scene}.pbrt'
    original = source.read_bytes()
    variants, commands, outputs, elapsed = [], [], {}, []
    start = time.monotonic()
    planes = ('clamped', 'raw') if backend == 'pbrt' else ('clamped',)
    for plane in planes:
        output = (path.parent / f'{plane}.exr').resolve()
        variant = source.with_name(f'quality-{backend}-{scene}-{spp}-{seed}-{plane}.pbrt')
        changed = variant_source(original, scene=scene, spp=spp, seed=seed,
                                 output=output, raw=plane == 'raw')
        variant.write_bytes(changed)
        variants.append({'path': str(variant), 'sha256': sha(variant),
                         'original_sha256': sha(source), 'plane': plane,
                         'transport_sha256': hashlib.sha256(replace_descriptor(changed, 'Film', b'Film')[0]).hexdigest()})
        binary = identity['binary']
        if backend == 'pbrt':
            command = [binary, '--nthreads', '16', '--seed', str(seed), '--render-coord-sys', 'world', '--outfile', str(output),
                       str(variant)]
        else:
            command = [binary, '--render-pbrt', str(variant), '--resource-profile', 'large',
                       '--output', str(output), '--raw-output', str(path.parent / 'raw.exr')]
            if backend == 'candidate':
                command += ['--material-seed', str(seed)]
        before = time.monotonic()
        log = path.parent / f'{plane}.log'
        with log.open('wb') as stream:
            subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True,
                           timeout=args.timeout, cwd=variant.parent)
        elapsed.append(time.monotonic() - before)
        commands.append(command)
        outputs[plane] = {'path': str(output), 'sha256': sha(output),
                          'scale': 100 / protocol['scenes'][scene]['iso'] if plane == 'raw' else 1}
    if backend != 'pbrt':
        raw = path.parent / 'raw.exr'
        outputs['raw'] = {'path': str(raw), 'sha256': sha(raw), 'scale': 1}
    if backend == 'pbrt' and len({v['transport_sha256'] for v in variants}) != 1:
        raise ValueError('PBRT raw companion changed transport')
    record = dict(expected, schema=1, status='complete', artifacts=outputs, variants=variants,
                  commands=commands, render_seconds=elapsed, end_to_end_seconds=time.monotonic()-start,
                  pbrt_raw_companion='Film clamp removed; ISO unchanged; decoded raw divided by ISO/100'
                  if backend == 'pbrt' else None)
    if args.metrics.is_file():
        record = extract_record(record, args.metrics)
    write_json(path, record)


def selected(protocol, args):
    for scene in args.scene or protocol['scenes']:
        for spp in args.spp or protocol['scenes'][scene]['budgets']:
            if spp not in protocol['scenes'][scene]['budgets']:
                raise ValueError('budget is not in the frozen protocol for ' + scene)
            for backend in args.backend or BACKENDS:
                seeds = protocol['seeds'][backend]
                for index in range(args.replicate_start, args.replicate_end):
                    yield backend, scene, spp, seeds[index]


def analyze(args, protocol):
    manifest = args.output_dir / 'metrics-input.txt'
    lines = []
    scenes = args.scene or list(protocol['scenes'])
    groups = []
    for scene in scenes:
        spp = protocol['scenes'][scene]['primary_spp']
        if args.spp and spp not in args.spp:
            continue
        for plane in protocol['planes']:
            groups.append((scene, spp, plane))
    if not groups:
        raise ValueError('no selected primary budgets to analyze')
    lines.append(f'PAVILION_METRICS_1 320 170 32 20000 {protocol["bootstrap"]["seed"]} '
                 f'{len(groups)} {len(protocol["regions"])} {args.threads}')
    for name, bounds in protocol['regions'].items():
        lines.append(name + ' ' + ' '.join(map(str, bounds)))
    all_records = []
    identities = {b: binary_identity(b, getattr(args, b), protocol) for b in BACKENDS}
    closure = corpus.load_manifest(ROOT / protocol['scene_manifest'])
    verify_closure(args.output_dir / 'staged' / closure['revision'], closure)
    for scene, spp, plane in groups:
        lines.append(f'{scene}/{plane}')
        for backend in BACKENDS:
            for seed in protocol['seeds'][backend]:
                path = record_path(args.output_dir, backend, scene, spp, seed)
                if not path.exists():
                    raise ValueError(f'missing required replica: {backend}/{scene}/{spp}/{seed}')
                record = validate_record(path, {'backend': backend, 'scene': scene, 'spp': spp,
                                                'sampler_seed': seed, 'renderer': identities[backend],
                                                'material_seed': seed if backend != 'baseline' else 'historical',
                                                'scene_manifest_sha256': sha(ROOT / protocol['scene_manifest'])})
                item = record['artifacts'][plane]
                # Native loading also validates FLOAT storage, dimensions and finiteness.
                lines.append(json.dumps(item['path']) + ' ' + str(item['scale']))
                all_records.append({'path': str(path), 'sha256': sha(path)})
    manifest.write_text('\n'.join(lines) + '\n')
    result = args.output_dir / 'quality-metrics.json'
    subprocess.run([str(args.metrics), 'analyze', str(manifest), str(result)], check=True,
                   timeout=args.analysis_timeout)
    value = json.loads(result.read_text())
    validate_analysis_result(value, protocol)
    value.update(protocol_sha256=PROTOCOL_SHA256, metrics_binary_sha256=sha(args.metrics),
                 records=all_records, metrics_input_sha256=sha(manifest))
    write_json(result, value)
    return value['accepted']


def validate_analysis_result(value, protocol):
    if value.get('bootstrap_draws') != 20000 or not math.isfinite(value.get('simultaneous_log_margin', math.nan)):
        raise ValueError('missing or inconclusive bootstrap bounds')
    gates = value.get('gates', [])
    if value.get('gate_count') != len(gates):
        raise ValueError('inconsistent native gate count')
    keys = set()
    passed = True
    for gate in gates:
        key = (gate['dataset'], gate['region'], gate['metric'])
        if key in keys or gate['region'] not in protocol['regions'] or gate['metric'] not in ('variance', 'tail'):
            raise ValueError('duplicate or invalid native gate')
        keys.add(key)
        for field in ('candidate', 'reference', 'epsilon', 'log_upper'):
            if not math.isfinite(gate[field]):
                raise ValueError('nonfinite native bound')
        if gate['candidate'] < 0 or gate['reference'] < 0 or gate['epsilon'] <= 0:
            raise ValueError('invalid native gate energy')
        absolute = gate['reference'] <= gate['epsilon']
        if gate['absolute'] != absolute:
            raise ValueError('incorrect near-zero absolute branch')
        upper = gate['upper']
        limit = 1.25 * gate['epsilon'] if absolute else 1.25
        pass_bound = upper is not None and math.isfinite(upper) and 0 <= upper <= limit
        if gate['pass'] != pass_bound:
            raise ValueError('inconsistent native failure bound')
        passed = passed and pass_bound
    means = value.get('mean_gates', [])
    mean_keys = {(g['dataset'], g['region'], g['channel']) for g in means}
    if len(mean_keys) != len(means):
        raise ValueError('duplicate native mean gate')
    expected = {(f'{scene}/{plane}', region, metric)
                for scene in protocol['scenes'] for plane in protocol['planes']
                for region in protocol['regions'] for metric in ('variance', 'tail')}
    expected_means = {(f'{scene}/raw', region, channel)
                      for scene in protocol['scenes'] for region in protocol['regions']
                      for channel in range(3)}
    complete = keys == expected and mean_keys == expected_means
    accepted = complete and passed and all(g['pass'] for g in means)
    if value.get('complete') != complete or value.get('accepted') != accepted:
        raise ValueError('native acceptance does not match all 48 bounds and RGB mean gates')
    return accepted


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('render', 'extract', 'analyze', 'reuse'))
    parser.add_argument('--output-dir', type=Path, default=ROOT / '.cache/pavilion-convergence')
    parser.add_argument('--scene-cache', type=Path, default=ROOT / '.cache/pbrt-pavilion')
    parser.add_argument('--candidate', type=Path, default=ROOT / 'build/debug/rypath')
    parser.add_argument('--baseline', type=Path, default=ROOT / '.cache/pavilion-convergence/baseline/rypath')
    parser.add_argument('--pbrt', type=Path, default=ROOT / '.cache/pbrt-reference/build/pbrt')
    parser.add_argument('--metrics', type=Path, default=ROOT / 'build/debug/rypath_pavilion_quality_metrics')
    parser.add_argument('--scene', choices=('day', 'night'), action='append')
    parser.add_argument('--backend', choices=BACKENDS, action='append')
    parser.add_argument('--spp', type=int, action='append')
    parser.add_argument('--replicate-start', type=int, default=0)
    parser.add_argument('--replicate-end', type=int, default=32)
    parser.add_argument('--reuse', action='store_true')
    parser.add_argument('--threads', type=int, default=min(16, os.cpu_count() or 1))
    parser.add_argument('--timeout', type=int, default=7200)
    parser.add_argument('--analysis-timeout', type=int, default=86400)
    args = parser.parse_args(argv)
    if not 0 <= args.replicate_start < args.replicate_end <= 32 or not 1 <= args.threads <= 64:
        parser.error('invalid replica range or thread count')
    args.output_dir = args.output_dir.resolve()
    args.metrics = args.metrics.resolve()
    return args


def main(argv=None):
    args = parse_args(argv)
    protocol = load_protocol()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    if args.mode == 'analyze':
        accepted = analyze(args, protocol)
        print('Complete protocol accepted' if accepted else 'Failed or incomplete protocol; acceptance unestablished')
        return 0 if accepted else 1
    manifest = corpus.load_manifest(ROOT / protocol['scene_manifest'])
    if manifest['revision'] != protocol['scene_revision']:
        raise ValueError('wrong scene revision')
    # A root provided directly at the revision is accepted as well as its cache parent.
    source = args.scene_cache
    if not (source / 'barcelona-pavilion').is_dir():
        source = source / manifest['revision']
    if args.mode == 'render':
        staged = stage_closure(source, args.output_dir / 'staged' / manifest['revision'], manifest)
        identities = {b: binary_identity(b, getattr(args, b), protocol) for b in args.backend or BACKENDS}
        for backend, scene, spp, seed in selected(protocol, args):
            render_one(args, protocol, manifest, staged, backend, scene, spp, seed, identities[backend])
    else:
        verify_closure(args.output_dir / 'staged' / manifest['revision'], manifest)
        identities = {b: binary_identity(b, getattr(args, b), protocol) for b in args.backend or BACKENDS}
        for backend, scene, spp, seed in selected(protocol, args):
            path = record_path(args.output_dir, backend, scene, spp, seed)
            record = validate_record(path, {'renderer': identities[backend], 'backend': backend,
                                            'scene': scene, 'spp': spp, 'sampler_seed': seed})
            if args.mode == 'extract':
                write_json(path, extract_record(record, args.metrics))
    print('Retained selected records; run analyze after all required replicates are complete')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
