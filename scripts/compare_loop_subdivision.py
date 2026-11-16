#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare deterministic Loop geometry with the exact pinned PBRT implementation.

Predeclared component tolerances are 2e-6 for positions and 3e-5 for raw normals,
each times max(1, abs(reference component)). Indices must agree exactly. Nonzero
normals must remain nonzero, with relative length error <=1e-4 and normalized
direction chord error <=2e-4.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import tempfile

from compare_hdr_io import build_probe
from compare_mesh_shapes import FIXTURES, ROOT, verify_reference

POSITION_TOLERANCE = 2e-6
NORMAL_TOLERANCE = 3e-5
NORMAL_LENGTH_TOLERANCE = 1e-4
NORMAL_DIRECTION_TOLERANCE = 2e-4


def control_meshes():
    meshes = {
        'tetra': ([[-1, -.8, 3], [1, -.8, 3], [0, 1, 3], [0, 0, 4.5]],
                  [0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3]),
        'boundary': ([[-1, -1, 3], [1, -1, 3], [-1, 1, 3], [1, 1, 3.8]],
                     [0, 2, 1, 1, 2, 3]),
    }
    for valence in (3, 5, 6, 7):
        points = [[0, 0, .2]] + [
            [math.cos(2 * math.pi * i / valence),
             math.sin(2 * math.pi * i / valence), 0]
            for i in range(valence)]
        indices = [v for i in range(valence)
                   for v in (0, i + 1, (i + 1) % valence + 1)]
        meshes[f'fan-{valence}'] = points, indices
    provenance = json.loads((FIXTURES/'provenance.json').read_text())
    for line in (6040, 6995):
        path = f'assets/bathroom-{line}.pbrt'
        record = next(r for r in provenance['external_assets'] if r['path'] == path)
        payload = (FIXTURES/path).read_bytes()
        if (len(payload) != record['bytes'] or
                hashlib.sha256(payload).hexdigest() != record['sha256']):
            raise ValueError('external control mesh checksum mismatch')
        source = payload.decode()
        xyz = list(map(float, re.search(
            r'"point3 P"\s*\[([^]]*)\]', source)[1].split()))
        indices = list(map(int, re.search(
            r'"integer indices"\s*\[([^]]*)\]', source)[1].split()))
        meshes[f'bathroom-{line}'] = [xyz[i:i+3] for i in range(0, len(xyz), 3)], indices
    return meshes


def probe_input(points, indices, levels):
    positions = '\n'.join(' '.join(map(str, p)) for p in points)
    topology = ' '.join(map(str, indices))
    return f'{levels} {len(points)} {len(indices)}\n{positions}\n{topology}\n'


def compare_geometry(ours, reference):
    if ours['indices'] != reference['indices']:
        raise ValueError('triangle indices differ')
    result = {}
    for field, tolerance in (('P', POSITION_TOLERANCE), ('N', NORMAL_TOLERANCE)):
        a, b = ours[field], reference[field]
        if not a or len(a) != len(b):
            raise ValueError(f'{field}: vertex count mismatch')
        worst = 0.
        for va, vb in zip(a, b):
            if len(va) != 3 or len(vb) != 3:
                raise ValueError(f'{field}: expected xyz triples')
            for x, y in zip(va, vb):
                if not math.isfinite(x) or not math.isfinite(y):
                    raise ValueError(f'{field}: nonfinite value')
                worst = max(worst, abs(x-y) / max(1, abs(y)))
        if worst > tolerance:
            raise ValueError(f'{field}: scaled error {worst} exceeds {tolerance}')
        result[field] = {'max_scaled_error': worst, 'tolerance': tolerance}
    max_length_error = max_direction_error = 0.
    for a, b in zip(ours['N'], reference['N']):
        na, nb = math.hypot(*a), math.hypot(*b)
        if nb == 0:
            if na != 0:
                raise ValueError('zero reference normal changed')
            continue
        if na == 0:
            raise ValueError('nonzero normal disappeared')
        relative = abs(na-nb) / nb
        direction = math.dist([x/na for x in a], [x/nb for x in b])
        if relative > NORMAL_LENGTH_TOLERANCE:
            raise ValueError('raw normal relative length differs')
        if direction > NORMAL_DIRECTION_TOLERANCE:
            raise ValueError('normal direction differs')
        max_length_error = max(max_length_error, relative)
        max_direction_error = max(max_direction_error, direction)
    result['N'].update(
        max_relative_length_error=max_length_error,
        max_direction_chord_error=max_direction_error,
        relative_length_tolerance=NORMAL_LENGTH_TOLERANCE,
        direction_chord_tolerance=NORMAL_DIRECTION_TOLERANCE)
    if len(ours['P']) != len(ours['N']):
        raise ValueError('position/normal count mismatch')
    if (not ours['indices'] or len(ours['indices']) % 3 or
            any(type(i) is not int or i < 0 or i >= len(ours['P'])
                for i in ours['indices'])):
        raise ValueError('invalid triangle indices')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rypath', type=Path, required=True,
                        help='rypath_loop_subdivision_dump')
    parser.add_argument('--pbrt', type=Path,
                        default=ROOT/'.cache/pbrt-reference/build/pbrt')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    reference = args.pbrt.resolve(strict=True)
    revision = verify_reference(reference)
    ours = args.rypath.resolve(strict=True)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='loop-oracle-', dir=args.output_dir.resolve()))
    upstream = work/'pbrt-loop-probe'
    source = ROOT/'scripts/pbrt_loop_probe.cpp'
    build_probe(reference.parent, 'pbrt', source, upstream, 'MESH_LOOP_ORACLE')
    summary = {
        'pbrt_revision': revision,
        'asset_provenance': json.loads((FIXTURES/'provenance.json').read_text()),
        'rypath_revision': subprocess.check_output(
            ['git', '-C', ROOT, 'rev-parse', 'HEAD'], text=True).strip(),
        'rypath_dirty': bool(subprocess.check_output(
            ['git', '-C', ROOT, 'status', '--porcelain'], text=True).strip()),
        'probe_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'passed': True,
        'complete': False,
        'tolerances': {
            'P': POSITION_TOLERANCE, 'N': NORMAL_TOLERANCE,
            'normal_relative_length': NORMAL_LENGTH_TOLERANCE,
            'normal_direction_chord': NORMAL_DIRECTION_TOLERANCE},
        'binaries': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in (ours, upstream)},
        'cases': {},
    }
    (work/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    for name, (points, indices) in control_meshes().items():
        levels = (0, 1, 2) if name.startswith('bathroom') else (-1, 0, 1, 2, 3)
        for level in levels:
            case = f'{name}-level{level}'
            payload = probe_input(points, indices, level)
            (work/(case+'.input.txt')).write_text(payload)
            meshes = {}
            try:
                for backend, binary in (('rypath', ours), ('pbrt', upstream)):
                    run = subprocess.run([str(binary)], input=payload,
                                         capture_output=True, text=True, timeout=120)
                    (work/(case+'-'+backend+'.stdout.json')).write_text(run.stdout)
                    (work/(case+'-'+backend+'.stderr.log')).write_text(run.stderr)
                    (work/(case+'-'+backend+'.process.json')).write_text(json.dumps(
                        {'command': [str(binary)], 'returncode': run.returncode},
                        indent=2)+'\n')
                    run.check_returncode()
                    meshes[backend] = json.loads(run.stdout)
                result = compare_geometry(meshes['rypath'], meshes['pbrt'])
                expected = len(indices) * 4**max(0, level)
                if len(meshes['rypath']['indices']) != expected:
                    raise ValueError('wrong refinement face count')
                summary['cases'][case] = {
                    'passed': True, 'triangles': expected // 3,
                    'vertices': len(meshes['rypath']['P']), **result}
                print(case+': PASS', flush=True)
            except (ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
                summary['passed'] = False
                summary['cases'][case] = {'passed': False, 'error': str(error)}
                print(case+': FAIL: '+str(error), flush=True)
            (work/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    summary['complete'] = True
    (work/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(work)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
