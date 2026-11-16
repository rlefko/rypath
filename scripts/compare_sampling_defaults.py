#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare randomized sampling/Gaussian images and paired descriptor defaults."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time

import compare_pbrt as comparison
from compare_box_filters import statistics_record, GATE
from compare_dielectrics import rypath_artifact_hashes
from compare_mesh_shapes import verify_reference
from extract_pbrt_rgb import REVISION

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT/'tests/fixtures/pbrt-render/sampling-defaults'
SAMPLERS = ('halton-permutedigits', 'zsobol-permutedigits',
            'zsobol-fastowen', 'zsobol-owen')
CONFIGURATIONS = tuple((scene, sampler) for scene in ('edge', 'diffuse-area', 'lens-texture')
                       for sampler in SAMPLERS) + (
    ('volume', 'halton-permutedigits'), ('volume', 'zsobol-fastowen'),
    ('cornell', 'halton-permutedigits'), ('cornell', 'zsobol-owen'),
    ('edge-coarse', 'halton-permutedigits'), ('edge-coarse', 'zsobol-fastowen'))
CASES = tuple(f'{scene}-{sampler}' for scene, sampler in CONFIGURATIONS)
DEFAULT_SCENES = ('implicit-defaults', 'explicit-defaults')
SCENES = tuple(dict.fromkeys(scene for scene, _ in CONFIGURATIONS))
SEED_TOKEN = b'"integer seed" [0]'


def staged_source(name):
    if name not in (*SCENES, *DEFAULT_SCENES):
        raise ValueError('unknown sampling/defaults fixture')
    source = (FIXTURES/(name+'.pbrt')).read_bytes()
    record = json.loads((FIXTURES/'manifest.json').read_text())[name]
    if len(source) != record['bytes'] or hashlib.sha256(source).hexdigest() != record['sha256']:
        raise ValueError(f'{name}: fixture size or checksum mismatch')
    return source


def case_source(case):
    if case not in CASES:
        raise ValueError('unknown sampling/defaults comparison case')
    scene, sampler = CONFIGURATIONS[CASES.index(case)]
    kind, randomization = sampler.split('-', 1)
    source = staged_source(scene)
    original = b'Sampler "halton" "string randomization" ["permutedigits"]'
    if source.count(original) != 1:
        raise ValueError('expected one declared base sampler')
    source = source.replace(original,
        f'Sampler "{kind}" "string randomization" ["{randomization}"]'.encode(), 1)
    if source.count(SEED_TOKEN) != 1 or source.count(b'"integer pixelsamples" [4096]') != 1:
        raise ValueError('expected one seed and 4096 authored samples')
    return scene, source


def render_pfm(backend, binary, variant, source, *, runner=None):
    """Set only reference coordinates/threads; preserve sampler and authored defaults."""
    if backend not in ('rypath', 'pbrt'):
        raise ValueError('unknown backend')
    execute = runner or subprocess.run

    def world_coordinates(command, **kwargs):
        if backend == 'pbrt':
            command[1:1] = ['--render-coord-sys', 'world', '--nthreads', '1']
        return execute(command, **kwargs)

    return comparison.render_pfm(backend, binary, variant, source, runner=world_coordinates)


def render_case(case, binary, reference, work, *, renderer=None):
    scene, source = case_source(case)
    execute = renderer or render_pfm
    measurements = {}
    for backend, executable, start in (('rypath', binary, 0), ('pbrt', reference, 16)):
        destination = work/case/backend
        destination.mkdir(parents=True)
        comparison.stage_dependencies(FIXTURES/(scene+'.pbrt'), destination)
        measurements[backend] = {name: [] for name in comparison.QUADRANTS}
        for seed in range(start, start+16):
            variant = source.replace(SEED_TOKEN, f'"integer seed" [{seed}]'.encode(), 1)
            pixels = execute(backend, executable, destination/f'{seed:02}.pbrt', variant)
            for region, bounds in comparison.QUADRANTS.items():
                measurements[backend][region].append(comparison.roi_means(pixels, bounds))
    return measurements


def check_defaults(binary, reference, work, *, renderer=None):
    """Exact within-backend omission equivalence at the authored 16-spp default.

    No cross-backend confidence interval is inferred from this deterministic pair.
    Film is explicitly bounded to 8x8; omission of Film is a portable translator test.
    """
    execute = renderer or render_pfm
    records = {}
    for backend, executable in (('rypath', binary), ('pbrt', reference)):
        destination = work/'defaults'/backend
        destination.mkdir(parents=True)
        images = {}
        for name in DEFAULT_SCENES:
            source = staged_source(name)
            comparison.stage_dependencies(FIXTURES/(name+'.pbrt'), destination)
            images[name] = execute(backend, executable, destination/(name+'.pbrt'), source)
        first, second = (images[name] for name in DEFAULT_SCENES)
        if len(first) != 64 or len(second) != 64:
            raise ValueError('expected complete 8x8 default images')
        full = comparison.roi_means(first, comparison.FULL)
        positive = any(value > 0 for value in full)
        records[backend] = {'passed': first == second and positive,
            'exact_pixel_equality': first == second, 'positive_signal': positive,
            'max_absolute_difference': max(abs(a-b) for x, y in zip(first, second)
                                           for a, b in zip(x, y)),
            'implicit_full_rgb': full, 'samples_per_pixel': 16}
    return records


def provenance_record(reference, binary, cases):
    records = {}
    closure = json.loads((FIXTURES/'dependencies.json').read_text())
    for case in cases:
        scene, source = case_source(case)
        records[case] = {'base_scene': scene,
            'authored_sha256': hashlib.sha256(staged_source(scene)).hexdigest(),
            'variant_sha256': hashlib.sha256(source).hexdigest(),
            'variant_parameters': {'sampler': CONFIGURATIONS[CASES.index(case)][1]},
            'dependencies': closure[scene]}
    return {'pbrt_revision': REVISION,
        'rypath_revision': subprocess.check_output(['git', '-C', ROOT, 'rev-parse', 'HEAD'], text=True).strip(),
        'rypath_dirty': bool(subprocess.check_output(['git', '-C', ROOT, 'status', '--porcelain'], text=True).strip()),
        'binaries': {str(reference): hashlib.sha256(reference.read_bytes()).hexdigest(),
                     **rypath_artifact_hashes(binary)},
        'resolution': [8, 8], 'randomized_samples_per_pixel': 4096,
        'seeds': {'rypath': list(range(16)), 'pbrt': list(range(16, 32))},
        'gate': GATE, 'cases': records,
        'default_equivalence': {'samples_per_pixel': 16, 'seed': 0,
            'claim': 'Exact within-backend implicit/explicit equivalence; no cross-backend statistical claim',
            'fixtures': {name: hashlib.sha256(staged_source(name)).hexdigest()
                         for name in DEFAULT_SCENES}},
        'deterministic_modes': 'Halton none/owen and unscrambled ZSobol use separate known-answer tests, not repeated-seed intervals',
        'reference_execution': {'render_coordinate_system': 'world', 'threads': 1},
        'asset_provenance': json.loads((FIXTURES/'provenance.json').read_text())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rypath', type=Path, required=True)
    parser.add_argument('--pbrt', type=Path, default=ROOT/'.cache/pbrt-reference/build/pbrt')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--defaults-only', action='store_true')
    args = parser.parse_args()
    if args.defaults_only and args.case:
        parser.error('--defaults-only cannot be combined with --case')
    binary, reference = args.rypath.resolve(strict=True), args.pbrt.resolve(strict=True)
    verify_reference(reference)
    selected = [] if args.defaults_only else list(dict.fromkeys(args.case or CASES))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='sampling-defaults-', dir=args.output_dir.resolve()))
    print(f'Artifacts: {work}', flush=True)
    provenance = provenance_record(reference, binary, selected)
    (work/'provenance.json').write_text(json.dumps(provenance, indent=2)+'\n')
    summary = {'gate': GATE, 'cases': {}, 'passed': True}
    start = time.perf_counter()
    for case in selected:
        measurements = render_case(case, binary, reference, work)
        gates = statistics_record(measurements)
        passed = all(record['passed'] for record in gates)
        summary['cases'][case] = {'passed': passed, 'regions': gates}
        summary['passed'] &= passed
        (work/(case+'-measurements.json')).write_text(json.dumps(measurements, indent=2)+'\n')
        (work/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
        print(f'{case}: {"PASS" if passed else "FAIL"}', flush=True)
    summary['default_equivalence'] = check_defaults(binary, reference, work)
    defaults_passed = all(record['passed'] for record in summary['default_equivalence'].values())
    summary['passed'] &= defaults_passed
    summary['seconds'] = time.perf_counter()-start
    (work/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(f'Default equivalence: {"PASS" if defaults_passed else "FAIL"}', flush=True)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
