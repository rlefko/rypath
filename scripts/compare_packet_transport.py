#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fixed-protocol packet transport comparisons with pinned CPU PBRT.

PBRT-only runs retain all sixteen replicates and make no agreement claim.
A later matched run can reuse those exact measurements with --reference-run.
The default ten cases use BDPT; optional coating controls record path or BDPT
explicitly and retain the original coated comparison as a separate case.
Finite film clamping is proposal dependent: its matched oracle uses only a
constant visible emitter at depth zero. Lens/splat comparisons stay unclamped.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
import traceback

import compare_pbrt as comparison
from compare_box_filters import GATE, statistics_record
from compare_dielectrics import rypath_artifact_hashes
from compare_mesh_shapes import verify_reference
from extract_pbrt_rgb import REVISION

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/fixtures/pbrt-render/packet-transport'
MANIFEST = FIXTURES / 'manifest.json'
CONFIG = json.loads(MANIFEST.read_text())
CASES = tuple(CONFIG['cases'])
DEFAULT_CASES = tuple(name for name in CASES if not CONFIG['cases'][name]['optional'])
REPLICATES = 16
SPP = 4096
TIMEOUT = 600
SEED_TOKEN = b'"integer seed" [0]'
SEEDS = {'rypath': list(range(16)), 'pbrt': list(range(16, 32))}


def digest(payload):
    return hashlib.sha256(payload).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')


def fixture_path(case):
    if case not in CASES:
        raise ValueError('unknown packet comparison case')
    path = ROOT / CONFIG['cases'][case]['source']
    if not path.resolve().is_relative_to(ROOT / 'tests/fixtures/pbrt-render'):
        raise ValueError('fixture escapes render fixture directory')
    return path


def staged_source(case):
    source = fixture_path(case).read_bytes()
    record = CONFIG['cases'][case]
    if len(source) != record['bytes'] or digest(source) != record['sha256']:
        raise ValueError(f'{case}: source size or checksum mismatch')
    return source


def replace_descriptor(source, directive, replacement):
    pattern = re.compile(rb'^' + directive.encode() + rb'\s+"[^"\n]+".*?'
                         rb'(?=^[A-Z][A-Za-z]*(?:\s|$)|\Z)', re.M | re.S)
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise ValueError(f'expected exactly one {directive} descriptor')
    original = matches[0].group()
    return pattern.sub(lambda _: replacement + b'\n', source, count=1), original


def case_source(case):
    """Preserve world/material/light data; record every changed render descriptor."""
    source = staged_source(case)
    original_world = source.split(b'WorldBegin', 1)[1]
    record = CONFIG['cases'][case]
    integrator = record.get('integrator', 'bdpt')
    if integrator not in ('bdpt', 'path'):
        raise ValueError(f'{case}: unsupported comparison integrator: {integrator!r}')
    overrides = []
    replacements = [
        ('Integrator', f'Integrator "{integrator}" "integer maxdepth" [{record["depth"]}]'.encode()),
        ('Sampler', f'Sampler "independent" "integer pixelsamples" [{SPP}] '
                    '"integer seed" [0]'.encode()),
    ]
    if record['gaussian_override']:
        replacements.append(('PixelFilter', b'PixelFilter "gaussian" "float xradius" [1.5] '
                             b'"float yradius" [1.5] "float sigma" [.5]'))
    for directive, replacement in replacements:
        source, authored = replace_descriptor(source, directive, replacement)
        if authored.strip() != replacement:
            overrides.append({'directive': directive, 'authored': authored.decode().strip(),
                              'comparison': replacement.decode()})
    if source.split(b'WorldBegin', 1)[1] != original_world:
        raise ValueError('comparison changed authored world data')
    for field, value in (('xresolution', 8), ('yresolution', 8)):
        pattern = rb'"integer ' + field.encode() + rb'"\s*\[\s*' + str(value).encode() + rb'\s*\]'
        if len(re.findall(pattern, source)) != 1:
            raise ValueError('expected authored 8x8 resolution')
    if source.count(SEED_TOKEN) != 1:
        raise ValueError('expected one independent seed')
    return source, overrides


def stage_dependencies(case, destination):
    """Use only the pinned closure; relocated variants keep entry-relative assets."""
    destination.mkdir(parents=True, exist_ok=True)
    source = fixture_path(case)
    for record in CONFIG['cases'][case]['dependencies']:
        relative = Path(record['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('dependency escapes entry directory')
        payload = (source.parent / relative).read_bytes()
        if len(payload) != record['bytes'] or digest(payload) != record['sha256']:
            raise ValueError(f'{case}: dependency size or checksum mismatch: {relative}')
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)


def render_pfm(backend, binary, variant, source, *, runner=None):
    """Reuse checked PFM/log handling, extending cold-pipeline timeout to 600s."""
    execute = runner or subprocess.run
    attempted = []

    def run(command, **kwargs):
        if backend == 'pbrt':
            command[1:1] = ['--render-coord-sys', 'world', '--nthreads', '1']
        attempted[:] = command
        kwargs['timeout'] = TIMEOUT
        return execute(command, **kwargs)

    try:
        return comparison.render_pfm(backend, binary, variant, source, runner=run)
    except OSError as error:
        # The shared helper records nonzero exits, reported ERRORs and timeouts.
        # Also retain launch/I/O failures, even when there was no child process.
        variant.with_suffix('.stderr.log').write_text(str(error) + '\n')
        variant.with_suffix('.stdout.log').write_text('')
        write_json(variant.with_suffix('.process.json'),
                   {'command': attempted, 'launch_error': str(error)})
        raise


def provenance(binary, reference, selected):
    cases = {}
    for case in selected:
        source, overrides = case_source(case)
        cases[case] = {**CONFIG['cases'][case], 'variant_sha256': digest(source),
                       'render_control_overrides': overrides,
                       'reference_cli_overrides': ['--render-coord-sys', 'world', '--nthreads', '1']}
    artifacts = {str(reference): digest(reference.read_bytes())}
    if binary:
        artifacts.update(rypath_artifact_hashes(binary))
    return {'pbrt_revision': REVISION,
            'rypath_revision': subprocess.check_output(['git', '-C', ROOT, 'rev-parse', 'HEAD'], text=True).strip(),
            'rypath_dirty': bool(subprocess.check_output(['git', '-C', ROOT, 'status', '--porcelain'], text=True).strip()),
            'binaries': artifacts, 'pbrt_binary_sha256': artifacts[str(reference)],
            'protocol': CONFIG['protocol'], 'timeout_seconds': TIMEOUT,
            'manifest_sha256': digest(MANIFEST.read_bytes()), 'cases': cases,
            'clamp_contract': 'Proposal-dependent biased film transform. Only visible-clamp at depth zero has a matched finite-clamp expectation; all BDPT splat/lens cases are unclamped.',
            'coverage': 'Owned fixtures and labeled CC0 Cornell derivatives; no new unmodified upstream scene coverage'}


def verify_artifacts(record):
    for name, expected in record['binaries'].items():
        if digest(Path(name).read_bytes()) != expected:
            raise ValueError(f'render artifact changed during the run: {name}')


def reference_measurements(run, current, case):
    previous = json.loads((run / 'provenance.json').read_text())
    old_case = previous['cases'][case]
    new_case = current['cases'][case]
    # Scene text alone does not identify referenced images or other assets.
    # Compare the selected closure, not the whole manifest: adding unrelated
    # optional cases must not invalidate an otherwise identical reference.
    inputs = ('variant_sha256', 'dependencies', 'reference_cli_overrides')
    if (previous['pbrt_revision'] != current['pbrt_revision'] or
            previous['protocol'] != current['protocol'] or
            any(key not in old_case or key not in new_case or old_case[key] != new_case[key]
                for key in inputs)):
        raise ValueError('reference-run protocol or input mismatch')
    # PBRT pathname can move; its exact executable hash must remain in the record.
    if current['pbrt_binary_sha256'] != previous['pbrt_binary_sha256']:
        raise ValueError('reference-run PBRT binary mismatch')
    values = json.loads((run / f'{case}-measurements.json').read_text())['pbrt']
    if set(values) != set(comparison.QUADRANTS):
        raise ValueError('reference-run region mismatch')
    # Reuse the full statistical validator to reject incomplete/nonfinite data.
    statistics_record({'rypath': values, 'pbrt': values})
    return values


def render_backend(case, backend, binary, work, *, renderer=None):
    source, _ = case_source(case)
    destination = work / case / backend
    stage_dependencies(case, destination)
    values = {region: [] for region in comparison.QUADRANTS}
    execute = renderer or render_pfm
    for seed in SEEDS[backend]:
        variant = source.replace(SEED_TOKEN, f'"integer seed" [{seed}]'.encode(), 1)
        pixels = execute(backend, binary, destination / f'{seed:02}.pbrt', variant)
        if len(pixels) != 64:
            raise ValueError('incomplete 8x8 render')
        for region, bounds in comparison.QUADRANTS.items():
            values[region].append(comparison.roi_means(pixels, bounds))
        write_json(destination / 'measurements.json', values)
    if not any(channel > 0 for rgb in values['full'] for channel in rgb):
        raise ValueError(f'{case}: {backend} produced no positive signal')
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rypath', type=Path)
    parser.add_argument('--pbrt', type=Path, default=ROOT / '.cache/pbrt-reference/build/pbrt')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--pbrt-only', action='store_true')
    parser.add_argument('--reference-run', type=Path, help='reuse verified sixteen-replicate PBRT measurements')
    parser.add_argument('--keep-going', action='store_true', help='continue other cases after execution errors')
    args = parser.parse_args()
    if args.pbrt_only and args.reference_run:
        parser.error('--pbrt-only and --reference-run are mutually exclusive')
    if not args.pbrt_only and not args.rypath:
        parser.error('--rypath required for matched comparisons')
    selected = list(dict.fromkeys(args.case or DEFAULT_CASES))
    reference = args.pbrt.resolve(strict=True)
    verify_reference(reference)
    binary = args.rypath.resolve(strict=True) if args.rypath else None
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='packet-transport-', dir=args.output_dir.resolve()))
    print(f'Artifacts: {work}', flush=True)
    record = provenance(binary, reference, selected)
    if args.reference_run:
        record['reference_run'] = str(args.reference_run.resolve(strict=True))
    write_json(work / 'provenance.json', record)
    write_json(work / 'fixture-manifest.json', CONFIG)
    summary = {'mode': 'PBRT reference only; no agreement claim' if args.pbrt_only else 'matched comparison',
               'gate': GATE, 'cases': {}, 'passed': True, 'complete': False,
               'agreement_established': False}
    start = time.perf_counter()
    for case in selected:
        measurements = {}
        before = time.perf_counter()
        try:
            verify_artifacts(record)
            measurements['pbrt'] = (reference_measurements(args.reference_run, record, case)
                                    if args.reference_run else render_backend(case, 'pbrt', reference, work))
            write_json(work / f'{case}-measurements.json', measurements)
            if binary and not args.pbrt_only:
                measurements['rypath'] = render_backend(case, 'rypath', binary, work)
                write_json(work / f'{case}-measurements.json', measurements)
                gates = statistics_record(measurements)
                summary['cases'][case] = {'passed': all(g['passed'] for g in gates), 'regions': gates}
            else:
                summary['cases'][case] = {'passed': True, 'reference_complete': True,
                                           'replicates': REPLICATES}
            verify_artifacts(record)
            summary['passed'] &= summary['cases'][case]['passed']
        except Exception as error:
            summary['passed'] = False
            summary['cases'][case] = {'passed': False, 'error': str(error),
                                       'error_type': type(error).__name__}
            (work / f'{case}-error.log').write_text(traceback.format_exc())
            if not args.keep_going:
                summary['cases'][case]['seconds'] = time.perf_counter() - before
                write_json(work / 'summary.json', summary)
                print(f'{case}: ERROR; retained logs', flush=True)
                return 1
        summary['cases'][case]['seconds'] = time.perf_counter() - before
        write_json(work / 'summary.json', summary)
        print(f'{case}: {"PASS" if summary["cases"][case]["passed"] else "FAIL"}', flush=True)
    summary['complete'] = True
    summary['agreement_established'] = summary['passed'] and not args.pbrt_only
    summary['seconds'] = time.perf_counter() - start
    write_json(work / 'summary.json', summary)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
