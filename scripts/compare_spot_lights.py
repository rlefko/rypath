#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare owned spot-light images with the pinned PBRT v4 integrator."""
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
FIXTURES = ROOT/'tests/fixtures/pbrt-render/spot-lights'
SCENES = ('penumbra', 'spectral-power', 'affine-transform', 'mixed', 'rough-reflection', 'rough-transmission', 'volume', 'external-quad')
GATE = '5 combined standard errors + 2e-5 * max(1, abs(reference))'


def staged_source(name):
    if name not in SCENES:
        raise ValueError('unknown spot comparison scene')
    source = (FIXTURES/(name+'.pbrt')).read_bytes()
    record = json.loads((FIXTURES/'manifest.json').read_text())[name]
    if len(source) != record['bytes'] or hashlib.sha256(source).hexdigest() != record['sha256']:
        raise ValueError(f'{name}: fixture size or checksum mismatch')
    return source


def backend_sources(name, source):
    """Preserve authored inputs; avoid a pinned PBRT affine light-bound defect."""
    sources = {'rypath': source, 'pbrt': source}
    overrides = {'rypath': [], 'pbrt': []}
    if name == 'affine-transform':
        token = b'Integrator "path"'
        if source.count(token) != 1 or b'"string lightsampler"' in source:
            raise ValueError('expected one unmodified path integrator')
        sources['pbrt'] = source.replace(
            token, token+b' "string lightsampler" "uniform"', 1)
        overrides['pbrt'] = [{
            'parameter': 'Integrator.lightsampler', 'authored': 'bvh (default)',
            'comparison': 'uniform',
            'reason': 'Pinned PBRT SpotLight::Bounds retains circular world cone '
                      'angles under affine scale/shear; BVHLightSampler can reject '
                      'valid SampleLi directions. Uniform preserves direct-light support.'}]
    return sources, overrides


def render_backend_sources(binary, reference, fixture, work, sources):
    """Match the shared seed/region protocol with explicit per-backend inputs."""
    measurements = {}
    for backend, executable, start in (('rypath', binary, 0), ('pbrt', reference, 16)):
        destination = work/fixture.stem/backend
        destination.mkdir(parents=True)
        comparison.stage_dependencies(fixture, destination)
        measurements[backend] = {name: [] for name in comparison.QUADRANTS}
        source = sources[backend]
        if source.count(b'"integer seed" [0]') != 1:
            raise ValueError('expected exactly one independent sampler seed')
        for seed in range(start, start+16):
            pixels = comparison.render_pfm(backend, executable, destination/f'{seed:02}.pbrt',
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
        if len(ours) < 8 or len(ours) != len(reference):
            raise ValueError('expected at least eight equally sized independent seed sets')
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
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rypath', type=Path, help='rypath_pbrt_reference executable')
    parser.add_argument('--pbrt', type=Path, default=ROOT/'.cache/pbrt-reference/build/pbrt')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--scene', choices=SCENES, action='append')
    parser.add_argument('--replicates', type=int, default=16)
    parser.add_argument('--pbrt-only', action='store_true', help='validate inputs only; no agreement claim')
    args = parser.parse_args()
    if args.replicates != 16:
        parser.error('the bounded spot suite requires exactly sixteen independent replicates')
    if not args.pbrt_only and not args.rypath:
        parser.error('--rypath is required unless --pbrt-only is set')
    reference = args.pbrt.resolve(strict=True)
    revision = subprocess.check_output(['git', '-C', reference.parent.parent/'source', 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION:
        raise ValueError(f'expected PBRT {REVISION}, got {revision}')
    binary = args.rypath.resolve(strict=True) if args.rypath else None
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='spots-', dir=args.output_dir.resolve()))
    print(f'Artifacts: {work}', flush=True)
    comparison.REPLICATES = args.replicates
    provenance = {'mode': 'PBRT inputs only' if args.pbrt_only else 'matched comparison',
                  'pbrt_revision': revision,
                  'rypath_revision': subprocess.check_output(['git', '-C', ROOT, 'rev-parse', 'HEAD'], text=True).strip(),
                  'rypath_dirty': bool(subprocess.check_output(['git', '-C', ROOT, 'status', '--porcelain'], text=True).strip()),
                  'binaries': {str(reference): hashlib.sha256(reference.read_bytes()).hexdigest()},
                  'resolution': [8, 8], 'spp': 4096,
                  'seeds': {'rypath': list(range(args.replicates)), 'pbrt': list(range(args.replicates, 2*args.replicates))},
                  'gate': GATE, 'fixtures': {},
                  'asset_provenance': json.loads((FIXTURES/'provenance.json').read_text())}
    if binary:
        provenance['binaries'].update(rypath_artifact_hashes(binary))
    summary = {'mode': provenance['mode'], 'gate': GATE, 'scenes': {}, 'passed': True}
    start = time.perf_counter()
    for name in dict.fromkeys(args.scene or SCENES):
        source = staged_source(name)
        sources, overrides = backend_sources(name, source)
        provenance['fixtures'][name] = {'authored_sha256': hashlib.sha256(source).hexdigest(),
            'backends': {backend: {'comparison_sha256': hashlib.sha256(payload).hexdigest(),
                                   'overrides': overrides[backend]}
                         for backend, payload in sources.items()},
            'dependencies': json.loads((FIXTURES/'dependencies.json').read_text())[name]}
        (work/'provenance.json').write_text(json.dumps(provenance, indent=2)+'\n')
        before = time.perf_counter()
        if args.pbrt_only:
            destination = work/name
            destination.mkdir()
            comparison.stage_dependencies(FIXTURES/(name+'.pbrt'), destination)
            pixels = comparison.render_pfm('pbrt', reference, destination/'input.pbrt',
                sources['pbrt'].replace(b'"integer seed" [0]', f'"integer seed" [{args.replicates}]'.encode()))
            means = comparison.roi_means(pixels, comparison.FULL)
            if not any(v > 0 for v in means):
                raise ValueError(f'{name}: reference has no positive signal')
            summary['scenes'][name] = {'passed': True, 'input_rgb': means,
                                      'seconds': time.perf_counter()-before}
        else:
            measurements = render_backend_sources(
                binary, reference, FIXTURES/(name+'.pbrt'), work, sources)
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
