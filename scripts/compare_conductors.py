#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Pinned PBRT GGX conductor comparison; fixed seeds and existing raw PFM gates."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

import compare_pbrt as comparison
from extract_pbrt_rgb import REVISION

ROOT = Path(__file__).resolve().parents[1]
SCENES = ('smooth-cu-environment', 'smooth-ag-area', 'rough-au', 'anisotropic-cu',
          'numeric-file-optical', 'detail-alpha', 'anisotropy-rotated', 'roughness-graph')


def comparison_variant(scene, source):
    """Preserve native zero-roughness coverage; avoid pinned float checker NaNs.

    Both backends receive the identical declared override. Seeds and gates stay
    unchanged, and original/staged hashes are retained in the run provenance.
    """
    overrides = []
    if scene == 'roughness-graph':
        original = b'"float tex1" [0]'
        replacement = b'"float tex1" [0.01]'
        if source.count(original) != 1:
            raise ValueError('roughness-graph: expected one zero-valued checker branch')
        source = source.replace(original, replacement, 1)
        overrides.append({'original': original.decode(), 'replacement': replacement.decode(),
                          'reason': 'Pinned PBRT checker cancellation can make a zero endpoint negative before sqrt'})
    return source, overrides


def rypath_artifact_hashes(binary):
    """Hash the canonical host executable and its required default Metal library."""
    executable = binary.resolve(strict=True)
    library = executable.with_name('rypath.metallib').resolve(strict=True)
    return {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (executable, library)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rypath', type=Path)
    parser.add_argument('--pbrt', type=Path, default=ROOT/'.cache/pbrt-reference/build/pbrt')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--scene', choices=SCENES, action='append')
    parser.add_argument('--pbrt-only', action='store_true', help='validate reference inputs only; no agreement claim')
    args = parser.parse_args()
    if not args.pbrt_only and not args.rypath:
        parser.error('--rypath is required unless --pbrt-only is set')
    pbrt = args.pbrt.resolve(strict=True)
    source = pbrt.parent.parent/'source'
    revision = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION:
        raise ValueError(f'expected PBRT {REVISION}, got {revision}')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='conductors-', dir=args.output_dir.resolve()))
    print(f'Artifacts: {work}', flush=True)
    binary_hashes = {str(pbrt): hashlib.sha256(pbrt.read_bytes()).hexdigest()}
    if args.rypath:
        binary_hashes.update(rypath_artifact_hashes(args.rypath))
    (work/'provenance.json').write_text(json.dumps({
        'pbrt_revision': revision,
        'rypath_revision': subprocess.check_output(['git', '-C', ROOT, 'rev-parse', 'HEAD'], text=True).strip(),
        'rypath_dirty': bool(subprocess.check_output(['git', '-C', ROOT, 'status', '--porcelain'], text=True).strip()),
        'binaries': binary_hashes,
        'mode': 'PBRT inputs only' if args.pbrt_only else 'matched comparison',
        'seeds': {'rypath': list(range(16)), 'pbrt': list(range(16,32))},
        'resolution': [8,8], 'spp': 4096,
        'gate': '5 combined standard errors + 2e-5 * max(1, abs(reference))',
    }, indent=2)+'\n')
    passed = True
    fixture_records = {}
    for scene in dict.fromkeys(args.scene or SCENES):
        fixture = ROOT/'tests/fixtures/pbrt-render/conductor'/f'{scene}.pbrt'
        record = json.loads((fixture.parent/'manifest.json').read_text())[scene]
        payload = fixture.read_bytes()
        if len(payload) != record['bytes'] or hashlib.sha256(payload).hexdigest() != record['sha256']:
            raise ValueError(f'{scene}: fixture size or checksum mismatch')
        staged_source, overrides = comparison_variant(scene, payload)
        fixture_records[scene] = {'original_sha256': hashlib.sha256(payload).hexdigest(),
                                  'comparison_sha256': hashlib.sha256(staged_source).hexdigest(),
                                  'overrides': overrides}
        provenance_path = work/'provenance.json'
        provenance = json.loads(provenance_path.read_text())
        provenance['fixtures'] = fixture_records
        provenance_path.write_text(json.dumps(provenance, indent=2)+'\n')
        if args.pbrt_only:
            destination = work/scene
            destination.mkdir()
            comparison.stage_dependencies(fixture, destination)
            pixels = comparison.render_pfm('pbrt', pbrt, destination/'16.pbrt',
                staged_source.replace(b'"integer seed" [0]', b'"integer seed" [16]'))
            means = comparison.roi_means(pixels, comparison.FULL)
            if not any(value > 0 for value in means):
                raise ValueError(f'{scene}: reference has no positive signal')
            print(f'PASS PBRT input {scene}: RGB={means}', flush=True)
        else:
            measurements = comparison.render_replicates(args.rypath.resolve(), pbrt, fixture,
                                                        work, comparison.QUADRANTS,
                                                        source_override=staged_source)
            (work/f'{scene}-measurements.json').write_text(json.dumps(measurements, indent=2)+'\n')
            passed &= comparison.compare_regions(scene, measurements)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
