#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Matched linear image comparisons against pinned PBRT v4 surface materials."""
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
FIXTURES = ROOT/'tests/fixtures/pbrt-render/surface-reference'
SCENES = ('thindielectric', 'diffusetransmission', 'mix', 'coateddiffuse', 'coatedconductor', 'measured')
GATE = '5 combined standard errors + 2e-5 * max(1, abs(reference))'


def staged_source(name, measured_table):
    payload = (FIXTURES/(name+'.pbrt')).read_bytes()
    record = json.loads((FIXTURES/'manifest.json').read_text())[name]
    if len(payload) != record['bytes'] or hashlib.sha256(payload).hexdigest() != record['sha256']:
        raise ValueError(f'{name}: fixture size or checksum mismatch')
    if name == 'measured':
        if measured_table is None:
            raise ValueError('measured comparison requires --measured-table')
        path = measured_table.resolve(strict=True)
        if any(char in str(path) for char in ('"', '\\', '\n', '\r')):
            raise ValueError('table path cannot be represented as a literal PBRT string')
        if path.read_bytes()[:12] != b'tensor_file\0':
            raise ValueError('measured table must have the PBRT v4 tensor signature')
        payload = payload.replace(b'$MEASURED_TABLE', str(path).encode())
    return payload


def statistics_record(measurements):
    records = []
    for region, ours in measurements['rypath'].items():
        for channel, a, b in zip('RGB', zip(*ours), zip(*measurements['pbrt'][region])):
            mean_a, mean_b = statistics.mean(a), statistics.mean(b)
            se = math.sqrt(statistics.variance(a)/len(a) + statistics.variance(b)/len(b))
            delta = mean_a-mean_b
            limit = 5*se+2e-5*max(1, abs(mean_b))
            records.append({'region': region, 'channel': channel, 'rypath': mean_a, 'pbrt': mean_b,
                            'difference': delta, 'combined_se': se, 'threshold': limit,
                            'passed': abs(delta) <= limit})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rypath', type=Path, required=True, help='rypath_pbrt_reference executable')
    parser.add_argument('--pbrt', type=Path, default=ROOT/'.cache/pbrt-reference/build/pbrt')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--measured-table', type=Path, default=ROOT/'.cache/bsdf-tables/cm_white_spec.bsdf')
    parser.add_argument('--scene', choices=SCENES, action='append')
    parser.add_argument('--replicates', type=int, default=16, help='independent replicates per backend (at least 8)')
    args = parser.parse_args()
    if args.replicates < 8:
        parser.error('at least eight independent replicates are required')
    reference = args.pbrt.resolve(strict=True)
    binary = args.rypath.resolve(strict=True)
    revision = subprocess.check_output(['git', '-C', reference.parent.parent/'source', 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION:
        raise ValueError(f'expected PBRT {REVISION}, got {revision}')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='surface-materials-', dir=args.output_dir.resolve()))
    print(f'Artifacts: {work}', flush=True)
    comparison.REPLICATES = args.replicates
    provenance = {'pbrt_revision': revision,
                  'rypath_revision': subprocess.check_output(['git','-C',ROOT,'rev-parse','HEAD'], text=True).strip(),
                  'rypath_dirty': bool(subprocess.check_output(['git','-C',ROOT,'status','--porcelain'], text=True).strip()),
                  'binaries': {str(reference): hashlib.sha256(reference.read_bytes()).hexdigest(), **rypath_artifact_hashes(binary)},
                  'resolution': [8,8], 'spp': 4096, 'seeds': {'rypath': list(range(args.replicates)), 'pbrt': list(range(args.replicates, 2*args.replicates))},
                  'gate': GATE, 'fourier': 'Excluded: no legacy Fourier material in pinned PBRT v4', 'fixtures': {}}
    selected = tuple(dict.fromkeys(args.scene or SCENES))
    if 'measured' in selected:
        table = args.measured_table.resolve(strict=True)
        provenance['measured_asset'] = {'path': str(table), 'bytes': table.stat().st_size,
             'sha256': hashlib.sha256(table.read_bytes()).hexdigest(),
             'usage': 'unmodified cached tensor asset; owned wrapper does not establish source-scene compatibility'}
        asset_manifest = table.parent/'provenance.json'
        if asset_manifest.is_file():
            for record in json.loads(asset_manifest.read_text()).get('assets', []):
                if record.get('file') == table.name and record.get('sha256') == provenance['measured_asset']['sha256']:
                    provenance['measured_asset']['source_record'] = record
    summary = {'gate': GATE, 'scenes': {}, 'passed': True}
    start = time.perf_counter()
    for name in selected:
        source = staged_source(name, args.measured_table)
        provenance['fixtures'][name] = {'authored_sha256': hashlib.sha256((FIXTURES/(name+'.pbrt')).read_bytes()).hexdigest(),
             'comparison_sha256': hashlib.sha256(source).hexdigest(), 'overrides': ['measured filename resolved identically for both renderers'] if name == 'measured' else []}
        (work/'provenance.json').write_text(json.dumps(provenance, indent=2)+'\n')
        before = time.perf_counter()
        measurements = comparison.render_replicates(binary, reference, FIXTURES/(name+'.pbrt'), work, comparison.QUADRANTS, source_override=source)
        records = statistics_record(measurements)
        passed = comparison.compare_regions(name, measurements)
        summary['scenes'][name] = {'seconds': time.perf_counter()-before, 'passed': passed, 'regions': records}
        summary['passed'] &= passed
        summary['seconds'] = time.perf_counter()-start
        (work/(name+'-measurements.json')).write_text(json.dumps(measurements, indent=2)+'\n')
        (work/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
        print(f'{name}: {"PASS" if passed else "FAIL"}, {summary["scenes"][name]["seconds"]:.2f}s', flush=True)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
