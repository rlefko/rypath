#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Two-arm serial timing companion; cannot substitute for quality-v2 replicas."""
from __future__ import annotations
import argparse
import datetime
import json
import os
import statistics
from pathlib import Path

import benchmark_pavilion_quality as inherited
import compare_pavilion_quality_v2 as quality


def order(protocol):
    quality.require(protocol['timing']['repetitions'] == 5,
                    'five measured alternating pairs required')
    for scene, settings in protocol['scenes'].items():
        for spp in settings['budgets']:
            for repetition in range(6):
                label = 'warmup' if repetition == 0 else 'measured'
                arms = (('reference', 'candidate') if repetition and repetition % 2 == 0
                        else ('candidate', 'reference'))
                for arm in arms:
                    yield arm, scene, spp, label, repetition


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--protocol', type=Path, default=quality.PROTOCOL)
    p.add_argument('--protocol-sha', required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--scene-cache', type=Path, default=quality.ROOT / '.cache/pbrt-pavilion')
    p.add_argument('--root-go', action='store_true')
    p.add_argument('--timeout', type=int, default=7200)
    a = p.parse_args(argv)
    protocol = quality.load_protocol(a.protocol, a.protocol_sha)
    quality.require(a.root_go and 0 < a.timeout <= 86400, 'root GO and bounded timeout required')
    quality.require(all(0 not in protocol['seed_ids'][arm] for arm in quality.ARMS),
                    'timing seed0 must not consume a reserved quality ID')
    quality.require(not a.output_dir.exists(), 'fresh timing directory required')
    quality.require(not any(k.startswith(('MTL_DEBUG', 'MTL_GPU_VALIDATION',
                                         'MTL_SHADER_VALIDATION', 'RYPATH_NATIVE_HINT',
                                         'RYPATH_DAY_MAP')) for k in os.environ),
                    'uncontrolled validation/experimental environment')
    quality.idle()
    identities = {arm: quality.artifact_identity(protocol, arm) for arm in quality.ARMS}
    manifest = quality.inherited.corpus.load_manifest(quality.ROOT / protocol['scene_manifest'])
    source = a.scene_cache.resolve()
    if not (source / 'barcelona-pavilion').is_dir():
        source /= manifest['revision']
    a.output_dir = a.output_dir.resolve()
    a.output_dir.mkdir(parents=True)
    staged = quality.inherited.stage_closure(source, a.output_dir / 'staged' / manifest['revision'], manifest)
    record = {'kind': 'quality-v2-timing-only', 'status': 'running',
              'protocol_sha256': a.protocol_sha, 'quality_replica_admission': False,
              'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'order': list(order(protocol)), 'rows': []}
    persist = lambda: quality.write_json(a.output_dir / 'timing-summary.json', record)
    persist()
    try:
        for arm, scene, spp, label, repetition in record['order']:
            folder = a.output_dir / scene / str(spp) / f'{label}-{repetition}-{arm}'
            folder.mkdir(parents=True)
            row = {'arm': arm, 'scene': scene, 'spp': spp, 'label': label,
                   'repetition': repetition, 'sampler_seed': 0, 'material_seed': 0,
                   'renderer': identities[arm], 'processes': []}
            record['rows'].append(row)
            persist()
            source = staged / 'barcelona-pavilion' / f'pavilion-{scene}.pbrt'
            for plane in ('clamped', 'raw') if arm == 'reference' else ('clamped',):
                output = folder / (plane + '.exr')
                variant = source.with_name(f'timing-v2-{scene}-{spp}-{label}-{repetition}-{arm}-{plane}.pbrt')
                quality.require(not variant.exists(), 'timing variant exists')
                variant.write_bytes(quality.inherited.variant_source(source.read_bytes(), scene=scene,
                                   spp=spp, seed=0, output=output, raw=plane == 'raw'))
                if arm == 'reference':
                    command = [identities[arm]['binary'], '--seed', '0', '--nthreads', '16',
                               '--render-coord-sys', 'world', '--outfile', str(output), str(variant)]
                else:
                    command = [identities[arm]['binary'], '--render-pbrt', str(variant),
                               '--resource-profile', 'large', '--output', str(output),
                               '--raw-output', str(folder / 'raw.exr'), '--material-seed', '0',
                               '--batch-size', '8']
                process = {'plane': plane}
                row['processes'].append(process)
                persist()
                timefile = folder / (plane + '.time')
                quality.idle()
                quality.owned_process(['/usr/bin/time', '-l', '-o', str(timefile), *command],
                                      folder / (plane + '.log'), cwd=variant.parent,
                                      timeout=a.timeout, row=process, persist=persist)
                quality.idle()
                quality.reject_log(process['log'])
                process['time'] = inherited.parse_time(timefile.read_text())
                process['phases'] = inherited.parse_phases(Path(process['log']).read_text(),
                                                            'candidate' if arm == 'candidate' else 'pbrt')
                process['timing_sha256'] = quality.sha(timefile)
                persist()
            row['artifacts'] = {}
            for plane in ('raw', 'clamped'):
                output = folder / (plane + '.exr')
                row['artifacts'][plane] = {'path': str(output), 'sha256': quality.sha(output),
                    'scale': quality.required_scale(arm, scene, plane, protocol),
                    'image': quality.read_image(output)}
            row['total_raw_and_clamped_seconds'] = sum(x['elapsed_seconds'] for x in row['processes'])
            row['peak_rss_bytes'] = max(x['time']['peak_rss_bytes'] for x in row['processes'])
            row['status'] = 'complete-timing-only'
            for current in quality.ARMS:
                quality.require(quality.artifact_identity(protocol, current) == identities[current],
                                'timing artifact identity changed')
            persist()
        quality.idle()
        summaries = []
        for scene, settings in protocol['scenes'].items():
            for spp in settings['budgets']:
                for arm in quality.ARMS:
                    measured = [r for r in record['rows'] if r['scene'] == scene and
                                r['spp'] == spp and r['arm'] == arm and r['label'] == 'measured']
                    quality.require(len(measured) == 5, 'fixed five measured repeats required')
                    values = [r['total_raw_and_clamped_seconds'] for r in measured]
                    summaries.append({'arm': arm, 'scene': scene, 'spp': spp,
                        'wall_seconds': values, 'mean': statistics.mean(values),
                        'sample_SD': statistics.stdev(values), 'minimum': min(values),
                        'maximum': max(values)})
        record.update(status='complete-fixed-two-arm-timings', summaries=summaries)
    except BaseException as error:
        record.update(status='failed-preserved', error=repr(error),
                      requires_root_review=any(p.get('requires_root_review')
                        for row in record['rows'] for p in row['processes']))
        raise
    finally:
        persist()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
