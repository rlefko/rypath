#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Opt-in real-signal checkpoint/resume acceptance, using an independent wire decoder.

Pass Debug/Release or sanitizer binaries as --first/--second for cross-build
checks. No GPU availability skip is accepted here. Results and subprocess logs
remain in --output-dir. This is a bounded integration run, not a convergence test.
"""
import argparse
import dataclasses
import hashlib
import json
import math
import os
from pathlib import Path
import re
import selectors
import shutil
import signal
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct('<8s5I2Q')
METADATA = struct.Struct('<IIQQQIIiIdBBdIfff32sI')
PIXEL = struct.Struct('<6dQ20d')
TARGET = 256
SIDE = 32
TOLERANCE = 2e-5
SENTINEL = b'checkpoint cancellation must preserve this final image\n'
PROGRESS = re.compile(r'^Committed samples per pixel: ([0-9]+)/([0-9]+)$')
READY = re.compile(r'^Checkpoint saved: .+ \(([0-9]+)/([0-9]+) samples per pixel\)$')


@dataclasses.dataclass(frozen=True)
class Segment:
    begin: int
    end: int
    build: str
    device: str
    architecture: str
    os: str


@dataclasses.dataclass(frozen=True)
class Checkpoint:
    width: int
    height: int
    target: int
    batch: int
    seed: int
    sampler: int
    randomization: int
    material_seed: int
    model: int
    exposure: float
    packet: bool
    compatibility_output: bool
    max_component_value: float
    filter_kind: int
    filter_x_radius: float
    filter_y_radius: float
    filter_sigma: float
    identity: bytes
    segments: tuple
    pixels: tuple
    raw_pixels: bytes

    @property
    def count(self):
        return self.pixels[0][6]


def decode(data):
    """Decode format-3 records independently, including the separate material seed."""
    if len(data) < HEADER.size + METADATA.size + 32:
        raise ValueError('truncated checkpoint')
    magic, *values = HEADER.unpack_from(data)
    versions, metadata_size, payload_size = values[:5], values[5], values[6]
    if magic != b'RYPCHK\r\n' or versions != [3, 12, 22, 7, 2]:
        raise ValueError('unsupported checkpoint header/contract')
    if (metadata_size < METADATA.size or metadata_size > 1024**2 or
            payload_size > 4 * 1024**3 or payload_size % PIXEL.size or
            len(data) != HEADER.size + metadata_size + payload_size + 32):
        raise ValueError('invalid checkpoint length')
    if hashlib.sha256(data[:-32]).digest() != data[-32:]:
        raise ValueError('checkpoint checksum mismatch')
    fields = METADATA.unpack_from(data, HEADER.size)
    (width, height, target, batch, seed, sampler, randomization, material_seed, model, exposure,
     packet, compatibility_output, clamp, filter_kind, xr, yr, sigma, identity, n) = fields
    if (not width or not height or not target or not batch or sampler not in range(4) or
            randomization not in range(4) or model not in (0, 1) or
            not math.isfinite(exposure) or exposure < 0 or
            (model == 0 and (exposure != 1 or packet)) or n > 1024 or
            packet not in (0, 1) or compatibility_output not in (0, 1) or
            not clamp > 0 or filter_kind not in range(3) or
            not all(math.isfinite(v) for v in (xr, yr, sigma)) or xr <= 0 or yr <= 0 or
            (filter_kind == 2 and sigma <= 0) or
            width * height * PIXEL.size != payload_size):
        raise ValueError('invalid checkpoint settings')
    integral = filter_integral(filter_kind, xr, yr, sigma)
    if packet and (not integral > 0 or not math.isfinite(integral)):
        raise ValueError('invalid packet reconstruction filter')
    metadata_end = HEADER.size + metadata_size
    at = HEADER.size + METADATA.size
    segments = []
    end = 0
    for _ in range(n):
        if at + 16 > metadata_end:
            raise ValueError('truncated provenance range')
        begin, stop = struct.unpack_from('<QQ', data, at)
        at += 16
        texts = []
        for _ in range(4):
            if at + 4 > metadata_end:
                raise ValueError('truncated provenance string')
            size, = struct.unpack_from('<I', data, at)
            at += 4
            if size > 4096 or at + size > metadata_end:
                raise ValueError('invalid provenance string size')
            try:
                texts.append(data[at:at + size].decode('utf-8'))
            except UnicodeDecodeError as error:
                raise ValueError('invalid provenance text') from error
            at += size
        if begin != end or stop <= begin or stop > target:
            raise ValueError('invalid provenance coverage')
        segments.append(Segment(begin, stop, *texts))
        end = stop
    if at != metadata_end:
        raise ValueError('trailing metadata')
    raw = data[metadata_end:-32]
    pixels = tuple(PIXEL.iter_unpack(raw))
    count = pixels[0][6]
    if count > target or end != count:
        raise ValueError('sample count/provenance mismatch')
    for pixel in pixels:
        floating = pixel[:6] + pixel[7:]
        pairs = [(pixel[start + c], pixel[start + c + 3])
                 for start in (0, 7, 13, 19) for c in range(3)]
        pairs.append((pixel[25], pixel[26]))
        if (pixel[6] != count or not all(math.isfinite(v) for v in floating) or
                not all(math.isfinite(a + b) for a, b in pairs) or
                (not count and any(v != 0 for v in floating)) or
                (packet and count and not pixel[25] + pixel[26] > 0)):
            raise ValueError('invalid pixel state')
    return Checkpoint(width, height, target, batch, seed, sampler, randomization, material_seed, model,
                      exposure, bool(packet), bool(compatibility_output), clamp, filter_kind,
                      xr, yr, sigma, identity, tuple(segments), pixels, raw)


def filter_integral(kind, x, y, sigma):
    if kind == 0:
        return 4 * x * y
    if kind == 1:
        return x * x * y * y
    def integral(radius):
        gaussian = math.exp(-radius * radius / (2 * sigma * sigma)) / (math.sqrt(2 * math.pi) * sigma)
        return math.erf(radius / (math.sqrt(2) * sigma)) - 2 * radius * gaussian
    return integral(x) * integral(y)


def read_checkpoint(path):
    if path.stat().st_size > 4 * 1024**3 + 1024**2 + HEADER.size + 32:
        raise ValueError('checkpoint exceeds wire size limit')
    return decode(path.read_bytes())


def compare(reference, resumed, exact):
    """Apply the fixed raw XYZ mean gate; same binary additionally requires bits."""
    keys = ('width', 'height', 'target', 'seed', 'sampler', 'randomization', 'material_seed', 'model',
            'exposure', 'identity', 'count', 'packet', 'compatibility_output',
            'max_component_value', 'filter_kind', 'filter_x_radius', 'filter_y_radius', 'filter_sigma')
    if any(getattr(reference, key) != getattr(resumed, key) for key in keys):
        raise ValueError('checkpoint identity/settings/count mismatch')
    if reference.count != reference.target:
        raise ValueError('comparison requires a completed target')
    worst = 0.0
    for a, b in zip(reference.pixels, resumed.pixels):
        # Check each raw/clamped camera/splat plane independently. A matching
        # combined image must not conceal a missing plane or changed weights.
        expected_values, actual_values = [], []
        for start in (0, 7, 13, 19):
            for c in range(3):
                expected_values.append((a[start + c] + a[start + c + 3]) / reference.count)
                actual_values.append((b[start + c] + b[start + c + 3]) / resumed.count)
        expected_values.append((a[25] + a[26]) / reference.count)
        actual_values.append((b[25] + b[26]) / resumed.count)
        for expected, actual in zip(expected_values, actual_values):
            normalized = abs(expected - actual) / max(1.0, abs(expected))
            worst = max(worst, normalized)
            if normalized > TOLERANCE:
                raise ValueError(f'raw XYZ/film accumulation differs: {normalized} > {TOLERANCE}')
    if exact and reference.raw_pixels != resumed.raw_pixels:
        raise ValueError('same-binary raw pixel state bytes differ')
    return worst


def verify_provenance(partial, resumed, reference, exact=False):
    if partial.identity != resumed.identity or partial.target != resumed.target:
        raise ValueError('resume changed checkpoint identity/target')
    if resumed.segments[:len(partial.segments)] != partial.segments:
        raise ValueError('resume changed inherited provenance')
    new = resumed.segments[len(partial.segments):]
    if len(new) != 1 or new[0].begin != partial.count or new[0].end != resumed.target:
        raise ValueError('resume provenance range is not the appended sample suffix')
    if len(reference.segments) != 1:
        raise ValueError('uninterrupted provenance is not a single segment')
    current = new[0]
    baseline = reference.segments[0]
    for segment in partial.segments + new + reference.segments:
        if not segment.build or not segment.device or not segment.os:
            raise ValueError('missing actual execution provenance')
    if exact:
        for segment in partial.segments:
            if (segment.build, segment.device, segment.architecture, segment.os) != (
                    baseline.build, baseline.device, baseline.architecture, baseline.os):
                raise ValueError('same-binary inherited provenance differs from baseline')
    if (current.build, current.device, current.architecture, current.os) != (
            baseline.build, baseline.device, baseline.architecture, baseline.os):
        raise ValueError('resumed suffix provenance differs from the second binary baseline')


def run(command, log, timeout=45, cancel_signal=None):
    """Drain a pipe with a deadline; signal on flushed readiness, never a sleep."""
    lines = []
    ready_count = None
    with subprocess.Popen([str(v) for v in command], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, start_new_session=True) as process:
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        pending = b''
        deadline = time.monotonic() + timeout
        try:
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise ValueError(f'render timed out after {timeout}s')
                for key, _ in selector.select(remaining):
                    block = os.read(key.fileobj.fileno(), 65536)
                    if not block:
                        selector.unregister(key.fileobj)
                        continue
                    pending += block
                    while b'\n' in pending:
                        raw, pending = pending.split(b'\n', 1)
                        line = raw.decode('utf-8', errors='replace')
                        lines.append(line)
                        progress = PROGRESS.fullmatch(line)
                        ready = progress or READY.fullmatch(line)
                        if cancel_signal is not None and ready_count is None and ready:
                            count, target = map(int, ready.groups())
                            if target != TARGET or count >= target or (progress and count == 0):
                                raise ValueError('invalid cancellation readiness count')
                            # The cross-build check must include a nonempty first-build
                            # prefix. Initial zero-state cancellation is tested in C++.
                            if not count:
                                continue
                            ready_count = count
                            process.send_signal(cancel_signal)
            if pending:
                lines.append(pending.decode('utf-8', errors='replace'))
            code = process.wait(timeout=max(0.01, deadline - time.monotonic()))
            expected = 128 + cancel_signal if cancel_signal is not None else 0
            if code != expected:
                raise ValueError(f'render exit {code}, expected {expected}: {command[0]}')
            if cancel_signal is not None and ready_count is None:
                raise ValueError('render exited without cancellation readiness')
            return ready_count
        finally:
            selector.close()
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            log.write_text('\n'.join(lines) + '\n')


def stage_fixture(directory, sampler):
    """Owned medium fixture plus an owned image map, fixed before execution."""
    source = (ROOT / 'tests/fixtures/pbrt-render/media-reference/isotropic.pbrt').read_text()
    source = source.replace('"integer xresolution" [8]', f'"integer xresolution" [{SIDE}]')
    source = source.replace('"integer yresolution" [8]', f'"integer yresolution" [{SIDE}]')
    source = source.replace('Sampler "independent" "integer pixelsamples" [4096] "integer seed" [0]',
                            f'Sampler "{sampler}" "integer pixelsamples" [{TARGET}] "integer seed" [37]')
    source = source.replace('Material "diffuse" "rgb reflectance" [.4 .3 .2]',
                            'Texture "panel" "spectrum" "imagemap" "string filename" "pattern.png"\n'
                            'Material "diffuse" "texture reflectance" "panel"')
    # Explicit UVs keep the image footprint in the exercised native path.
    source = source.replace('[-3 -3 5 3 -3 5 3 3 5 -3 3 5] "integer indices" [0 1 2 0 2 3]',
                            '[-3 -3 5 3 -3 5 3 3 5 -3 3 5] "integer indices" [0 1 2 0 2 3] '
                            '"point2 uv" [0 0 1 0 1 1 0 1]')
    path = directory / 'fixture.pbrt'
    path.write_text(source)
    shutil.copyfile(ROOT / 'tests/fixtures/pbrt-render/image-assets/asymmetric.png',
                    directory / 'pattern.png')
    return path


def command(binary, fixture, output, checkpoint, batch, resume=None):
    args = [binary, '--render-pbrt', fixture, '--output', output, '--batch-size', batch,
            '--checkpoint', checkpoint]
    if resume is not None:
        args += ['--resume', resume]
    return args


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--first', type=Path, default=ROOT / 'build/debug/rypath')
    parser.add_argument('--second', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args(argv)
    first = args.first.resolve()
    second = (args.second or first).resolve()
    first_hash = hashlib.sha256(first.read_bytes()).hexdigest()
    second_hash = hashlib.sha256(second.read_bytes()).hexdigest()
    exact = first_hash == second_hash
    args.output_dir.mkdir(parents=True, exist_ok=True)
    records = []
    for sampler in ('independent', 'sobol'):
        directory = args.output_dir / sampler
        directory.mkdir(exist_ok=True)
        fixture = stage_fixture(directory, sampler)
        baseline_path = directory / 'uninterrupted.checkpoint'
        run(command(second, fixture, directory / 'uninterrupted.png', baseline_path, 7),
            directory / 'uninterrupted.log')
        baseline = read_checkpoint(baseline_path)
        if baseline.sampler != (sampler == 'sobol') or baseline.target != TARGET:
            raise ValueError('baseline sampler/target differs from the fixed workload')
        for signum in (signal.SIGINT, signal.SIGTERM):
            prefix = directory / signal.Signals(signum).name.lower()
            partial_path = prefix.with_suffix('.partial.checkpoint')
            resumed_path = prefix.with_suffix('.resumed.checkpoint')
            output = prefix.with_suffix('.png')
            output.write_bytes(SENTINEL)
            readiness = run(command(first, fixture, output, partial_path, 1),
                            prefix.with_suffix('.cancel.log'), cancel_signal=signum)
            if output.read_bytes() != SENTINEL:
                raise ValueError('cancelled render replaced the final image sentinel')
            partial = read_checkpoint(partial_path)
            if not partial.count or partial.count < readiness or partial.count >= TARGET:
                raise ValueError('cancelled checkpoint has an invalid completed prefix')
            if readiness and not partial.count:
                raise ValueError('progress readiness did not preserve a committed batch')
            run(command(second, fixture, output, resumed_path, 5, partial_path),
                prefix.with_suffix('.resume.log'))
            if output.read_bytes() == SENTINEL:
                raise ValueError('completed resume did not publish final image')
            resumed = read_checkpoint(resumed_path)
            if resumed.batch != 5:
                raise ValueError('resume did not use the changed dispatch batch size')
            verify_provenance(partial, resumed, baseline, exact=exact)
            worst = compare(baseline, resumed, exact)
            records.append({'sampler': sampler, 'signal': signal.Signals(signum).name,
                            'partial_samples': partial.count, 'target': TARGET,
                            'identity': resumed.identity.hex(),
                            'raw_pixel_bytes_equal': baseline.raw_pixels == resumed.raw_pixels,
                            'provenance': [dataclasses.asdict(s) for s in resumed.segments],
                            'worst_normalized_xyz_mean_error': worst})
    report = {'first': str(first), 'second': str(second),
              'first_sha256': first_hash, 'second_sha256': second_hash,
              'mean_tolerance': TOLERANCE,
              'same_binary_requires_exact_pixel_state': exact, 'cases': records}
    (args.output_dir / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit(f'checkpoint validation failed: {error}') from error
