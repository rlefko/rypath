#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare box-filter radius images with the pinned PBRT v4 integrator."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

import compare_pbrt as comparison
from compare_dielectrics import rypath_artifact_hashes
from extract_pbrt_rgb import REVISION

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT/'tests/fixtures/pbrt-render/box-filters'
SCENES = ('pinhole-anisotropic', 'focus-plane', 'defocus', 'texture-filtered')
FILTERS = ('box',)
SAMPLERS = ('independent', 'sobol-fastowen')
CASES = tuple(f'{scene}-{pixel_filter}-{sampler}' for scene in SCENES
              for pixel_filter in FILTERS for sampler in SAMPLERS)


def case_source(case):
    """Apply only declared film-filter and sampler changes to a verified fixture."""
    if case not in CASES:
        raise ValueError('unknown box-filter comparison case')
    scene, pixel_filter, sampler = next((s, f, a) for s in SCENES for f in FILTERS
        for a in SAMPLERS if case == f'{s}-{f}-{a}')
    source = staged_source(scene)
    source = source.replace(b'"float xradius" [0.5]', b'"float xradius" [1.5]')
    source = source.replace(b'"float yradius" [0.5]', b'"float yradius" [0.75]')
    if sampler == 'sobol-fastowen':
        source = source.replace(b'Sampler "independent"',
            b'Sampler "sobol" "string randomization" ["fastowen"]')
    return scene, source

GATE = '5 combined standard errors + 2e-5 * max(1, abs(reference))'


def staged_source(name):
    if name not in SCENES:
        raise ValueError('unknown box-filter comparison scene')
    fixture_name = name
    source = (FIXTURES/(fixture_name+'.pbrt')).read_bytes()
    record = json.loads((FIXTURES/'manifest.json').read_text())[fixture_name]
    if len(source) != record['bytes'] or hashlib.sha256(source).hexdigest() != record['sha256']:
        raise ValueError(f'{name}: fixture size or checksum mismatch')
    return source


def backend_sources(name, source):
    """Preserve authored inputs and record the required reference CLI override."""
    if name not in SCENES:
        raise ValueError('unknown box-filter comparison scene')
    return {'rypath': source, 'pbrt': source}, {
        'rypath': [], 'pbrt': [{
            'parameter': '--render-coord-sys', 'authored': 'camera-world (PBRT default)',
            'comparison': 'world',
            'reason': 'Match world-space geometry and camera coordinates explicitly.'}]}


def render_pfm(backend, binary, variant, source, *, runner=None):
    """Inject the official PBRT CLI option; retain the actual command and logs."""
    execute = runner or subprocess.run

    def world_coordinates(command, **kwargs):
        # Mutate the shared command so its retained process record includes the
        # exact command executed, including this bounded backend-only override.
        if backend == 'pbrt':
            command[1:1] = ['--render-coord-sys', 'world', '--nthreads', '2']
        return execute(command, **kwargs)

    return comparison.render_pfm(backend, binary, variant, source, runner=world_coordinates)


def render_backend_sources(binary, reference, fixture, work, sources, *, case=None):
    """Match the shared seed/region protocol with explicit per-backend inputs."""
    measurements = {}
    for backend, executable, start in (('rypath', binary, 0), ('pbrt', reference, 16)):
        destination = work/(case or fixture.stem)/backend
        destination.mkdir(parents=True)
        comparison.stage_dependencies(fixture, destination)
        measurements[backend] = {name: [] for name in comparison.QUADRANTS}
        source = sources[backend]
        if source.count(b'"integer seed" [0]') != 1:
            raise ValueError('expected exactly one independent sampler seed')
        for seed in range(start, start+16):
            pixels = render_pfm(backend, executable, destination/f'{seed:02}.pbrt',
                source.replace(b'"integer seed" [0]', f'"integer seed" [{seed}]'.encode(), 1))
            for name, bounds in comparison.QUADRANTS.items():
                measurements[backend][name].append(comparison.roi_means(pixels, bounds))
    return measurements


def statistics_record(measurements):
    """Gate independent seed means; correlated pixels never count as replicates."""
    if set(measurements) != {'rypath', 'pbrt'}:
        raise ValueError('expected both backends')
    if not measurements['rypath'] or set(measurements['rypath']) != set(measurements['pbrt']):
        raise ValueError('expected matching nonempty regions')
    records = []
    for region, ours in measurements['rypath'].items():
        reference = measurements['pbrt'][region]
        if len(ours) != 16 or len(reference) != 16:
            raise ValueError('expected exactly sixteen independent seeds per backend')
        for values in (ours, reference):
            if any(len(rgb) != 3 or any(not math.isfinite(v) for v in rgb) for rgb in values):
                raise ValueError('expected finite RGB seed means')
        for channel, a, b in zip('RGB', zip(*ours), zip(*reference)):
            mean_a, mean_b = statistics.mean(a), statistics.mean(b)
            se = math.sqrt(statistics.variance(a)/len(a) + statistics.variance(b)/len(b))
            delta = mean_a-mean_b
            limit = 5*se+2e-5*max(1, abs(mean_b))
            records.append({'region': region, 'channel': channel, 'rypath': mean_a,
                            'pbrt': mean_b, 'difference': delta, 'combined_se': se,
                            'threshold': limit, 'passed': abs(delta) <= limit})
    for backend in ('rypath', 'pbrt'):
        if 'full' not in measurements[backend]:
            raise ValueError('expected full-image region for positive-signal validation')
        full = measurements[backend]['full']
        if sum(statistics.mean(channel) for channel in zip(*full)) <= 0:
            raise ValueError(f'{backend}: full-image mean has no positive signal')
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rypath', type=Path, help='rypath_pbrt_reference executable')
    parser.add_argument('--pbrt', type=Path, default=ROOT/'.cache/pbrt-reference/build/pbrt')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--scene', choices=SCENES, action='append')
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--replicates', type=int, default=16)
    parser.add_argument('--pbrt-only', action='store_true', help='validate inputs only; no agreement claim')
    args = parser.parse_args()
    selected = [case for case in dict.fromkeys(args.case or CASES)
                if not args.scene or case_source(case)[0] in args.scene]
    if not selected:
        parser.error('--case and --scene select no matching cases')
    if args.replicates != 16:
        parser.error('the bounded box-filter suite requires exactly sixteen independent replicates')
    if not args.pbrt_only and not args.rypath:
        parser.error('--rypath is required unless --pbrt-only is set')
    reference = args.pbrt.resolve(strict=True)
    revision = subprocess.check_output(['git', '-C', reference.parent.parent/'source', 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION:
        raise ValueError(f'expected PBRT {REVISION}, got {revision}')
    binary = args.rypath.resolve(strict=True) if args.rypath else None
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='box-filters-', dir=args.output_dir.resolve()))
    print(f'Artifacts: {work}', flush=True)
    comparison.REPLICATES = args.replicates
    provenance = {'mode': 'PBRT inputs only' if args.pbrt_only else 'matched comparison',
                  'pbrt_revision': revision,
                  'rypath_revision': subprocess.check_output(['git', '-C', ROOT, 'rev-parse', 'HEAD'], text=True).strip(),
                  'rypath_dirty': bool(subprocess.check_output(['git', '-C', ROOT, 'status', '--porcelain'], text=True).strip()),
                  'binaries': {str(reference): hashlib.sha256(reference.read_bytes()).hexdigest()},
                  'resolution': [8, 8], 'spp': 4096,
                  'seeds': {'rypath': [] if args.pbrt_only else list(range(args.replicates)),
                            'pbrt': [16] if args.pbrt_only else list(range(16, 32))},
                  'comparison_seed_schedule': {'rypath': list(range(16)), 'pbrt': list(range(16, 32))},
                  'gate': GATE, 'fixtures': {},
                  'cases': selected,
                  'filters': list(FILTERS), 'samplers': list(SAMPLERS),
                  'reference_execution': {'render_coordinate_system': 'world', 'threads': 2},
                  'asset_provenance': json.loads((FIXTURES/'provenance.json').read_text())}
    if binary:
        provenance['binaries'].update(rypath_artifact_hashes(binary))
    summary = {'mode': provenance['mode'], 'gate': GATE, 'scenes': {}, 'passed': True}
    start = time.perf_counter()
    for name in selected:
        scene, source = case_source(name)
        sources, overrides = backend_sources(scene, source)
        comparison.stage_dependencies(FIXTURES/(scene+'.pbrt'), work/'verified-assets')
        provenance['fixtures'][name] = {'authored_sha256': hashlib.sha256(staged_source(scene)).hexdigest(),
            'base_scene': scene, 'variant_sha256': hashlib.sha256(source).hexdigest(),
            'variant_parameters': {
                'pixel_filter': 'box',
                'filter_x_radius': 1.5, 'filter_y_radius': .75,
                'sampler': 'sobol' if name.endswith('sobol-fastowen') else 'independent',
                'randomization': 'fastowen' if name.endswith('sobol-fastowen') else None},
            'backends': {backend: {'comparison_sha256': hashlib.sha256(payload).hexdigest(),
                                   'overrides': overrides[backend]}
                         for backend, payload in sources.items()},
            'dependencies': json.loads((FIXTURES/'dependencies.json').read_text())[scene]}
        (work/'provenance.json').write_text(json.dumps(provenance, indent=2)+'\n')
        before = time.perf_counter()
        if args.pbrt_only:
            destination = work/name
            destination.mkdir()
            comparison.stage_dependencies(FIXTURES/(scene+'.pbrt'), destination)
            pixels = render_pfm('pbrt', reference, destination/'input.pbrt',
                sources['pbrt'].replace(b'"integer seed" [0]', f'"integer seed" [{args.replicates}]'.encode()))
            means = comparison.roi_means(pixels, comparison.FULL)
            if not any(v > 0 for v in means):
                raise ValueError(f'{name}: reference has no positive signal')
            summary['scenes'][name] = {'passed': True, 'input_rgb': means,
                                      'seconds': time.perf_counter()-before}
        else:
            measurements = render_backend_sources(
                binary, reference, FIXTURES/(scene+'.pbrt'), work, sources, case=name)
            records = statistics_record(measurements)
            passed = all(record['passed'] for record in records)
            comparison.compare_regions(name, measurements)
            summary['scenes'][name] = {'passed': passed, 'regions': records,
                                      'seconds': time.perf_counter()-before}
            summary['passed'] &= passed
            (work/(name+'-measurements.json')).write_text(json.dumps(measurements, indent=2)+'\n')
        summary['seconds'] = time.perf_counter()-start
        (work/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
        print(f'{name}: {"PASS" if summary["scenes"][name]["passed"] else "FAIL"}', flush=True)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
