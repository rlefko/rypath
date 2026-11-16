#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Portable checks on the independent acceptance decoder and execution gates."""
import dataclasses
import hashlib
from pathlib import Path
import signal
import struct
import sys
import tempfile
import unittest

from validate_checkpoints import (HEADER, METADATA, PIXEL, TARGET, Segment, command,
                                  compare, decode, run, stage_fixture, verify_provenance)


def wire(count=3, target=3, segments=None, sums=(2., 3., 4.), correction=(0., 0., 0.), exposure=1., model=1):
    if segments is None:
        segments = [] if not count else [Segment(0, count, 'build', 'device', 'arch', 'os')]
    metadata = METADATA.pack(1, 1, target, 1, 37, 0, 2, model, exposure, b'i' * 32, len(segments))
    for segment in segments:
        metadata += struct.pack('<QQ', segment.begin, segment.end)
        for text in (segment.build, segment.device, segment.architecture, segment.os):
            raw = text.encode()
            metadata += struct.pack('<I', len(raw)) + raw
    payload = PIXEL.pack(*sums, *correction, count)
    data = HEADER.pack(b'RYPCHK\r\n', 1, 8, 9, 2, 1, len(metadata), len(payload)) + metadata + payload
    return data + hashlib.sha256(data).digest()


def rehash(data):
    return bytes(data[:-32]) + hashlib.sha256(data[:-32]).digest()


class CheckpointValidatorTests(unittest.TestCase):
    def test_independent_wire_decoder_and_checksum(self):
        raw = wire(correction=(1e-16, -1e-16, 1e-17))
        result = decode(raw)
        self.assertEqual(result.count, 3)
        self.assertEqual(result.identity, b'i' * 32)
        self.assertEqual(result.raw_pixels, raw[-32 - PIXEL.size:-32])
        self.assertEqual(result.segments[0].device, 'device')
        bad = bytearray(raw)
        bad[-33] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum'):
            decode(bad)

    def test_truncation_versions_lengths_and_unconsumed_metadata(self):
        raw = wire()
        for cut in (0, 20, 43, len(raw) - 1):
            with self.subTest(cut=cut), self.assertRaises(ValueError):
                decode(raw[:cut])
        # Independently mutate all five authenticated contract words. In
        # particular, pre-lens renderer 8 / identity 7 / sampling 1 fail.
        for offset, version in ((8, 3), (12, 3), (12, 4), (12, 5), (12, 6), (12, 7), (16, 3), (16, 4), (16, 5), (16, 6), (16, 7), (16, 8), (20, 1), (20, 3), (24, 3)):
            with self.subTest(contract_offset=offset, version=version):
                bad = bytearray(raw)
                struct.pack_into('<I', bad, offset, version)
                with self.assertRaisesRegex(ValueError, 'contract'):
                    decode(rehash(bad))
        bad = bytearray(raw)
        struct.pack_into('<Q', bad, 28, 2**30)
        with self.assertRaisesRegex(ValueError, 'length'):
            decode(rehash(bad))
        bad = bytearray(raw)
        # Metadata's width must match the pixel payload even with a valid hash.
        struct.pack_into('<I', bad, HEADER.size, 2)
        with self.assertRaisesRegex(ValueError, 'settings'):
            decode(rehash(bad))
        bad = bytearray(raw)
        struct.pack_into('<I', bad, HEADER.size + METADATA.size + 16, 4097)
        with self.assertRaisesRegex(ValueError, 'string size'):
            decode(rehash(bad))

    def test_nonfinite_and_invalid_counts_fail(self):
        for kwargs in ({'sums': (float('nan'), 1., 1.)},
                       {'sums': (float('inf'), 1., 1.)},
                       {'count': 0}, {'count': 4, 'target': 3},
                       {'sums': (sys.float_info.max, 1., 1.),
                        'correction': (sys.float_info.max, 0., 0.)}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                decode(wire(**kwargs))
        empty = decode(wire(count=0, sums=(0., 0., 0.)))
        self.assertEqual(empty.count, 0)
        self.assertEqual(empty.segments, ())

    def test_exposure_tracks_current_codec_domain(self):
        self.assertEqual(decode(wire(exposure=0.)).exposure, 0.)
        self.assertEqual(decode(wire(exposure=1., model=0)).exposure, 1.)
        for exposure, model in ((-1., 1), (float('nan'), 1), (0., 0), (2., 0)):
            with self.subTest(exposure=exposure, model=model), self.assertRaises(ValueError):
                decode(wire(exposure=exposure, model=model))

    def test_provenance_ranges_and_counts(self):
        for segment in (Segment(1, 3, 'a', 'b', 'c', 'd'),
                        Segment(0, 0, 'a', 'b', 'c', 'd'),
                        Segment(0, 2, 'a', 'b', 'c', 'd')):
            with self.subTest(segment=segment), self.assertRaises(ValueError):
                decode(wire(segments=[segment]))

    def test_comparison_uses_mean_floor_and_exact_same_binary_state(self):
        reference = decode(wire())
        close = decode(wire(sums=(2. + 3e-5, 3., 4.)))
        self.assertLess(compare(reference, close, exact=False), 2e-5)
        with self.assertRaisesRegex(ValueError, 'state bytes'):
            compare(reference, close, exact=True)
        self.assertEqual(compare(reference, reference, exact=True), 0.)
        with self.assertRaisesRegex(ValueError, 'raw XYZ'):
            compare(reference, decode(wire(sums=(2. + 1e-3, 3., 4.))), exact=False)
        for key, value in (('identity', b'x' * 32), ('target', 4), ('seed', 38)):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'identity/settings'):
                compare(reference, dataclasses.replace(reference, **{key: value}), exact=False)

    def test_provenance_suffix_is_exact_and_covers_resume(self):
        prefix = Segment(0, 1, 'old', 'device', 'arch', 'os')
        suffix = Segment(1, 3, 'build', 'device', 'arch', 'os')
        partial = decode(wire(count=1, target=3, segments=[prefix]))
        resumed = decode(wire(segments=[prefix, suffix]))
        baseline = decode(wire())
        verify_provenance(partial, resumed, baseline)
        with self.assertRaisesRegex(ValueError, 'same-binary inherited'):
            verify_provenance(partial, resumed, baseline, exact=True)
        bad = dataclasses.replace(resumed, segments=(dataclasses.replace(prefix, build='edited'), suffix))
        with self.assertRaisesRegex(ValueError, 'inherited'):
            verify_provenance(partial, bad, baseline)
        bad = dataclasses.replace(resumed, segments=(prefix, dataclasses.replace(suffix, build='other')))
        with self.assertRaisesRegex(ValueError, 'second binary'):
            verify_provenance(partial, bad, baseline)
        empty = decode(wire(count=0, target=3, sums=(0., 0., 0.)))
        verify_provenance(empty, baseline, baseline)

    def test_staged_fixture_has_real_image_and_volume_with_fixed_budget(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = stage_fixture(Path(temporary), 'sobol')
            text = path.read_text()
            for token in ('Sampler "sobol"', f'"integer pixelsamples" [{TARGET}]',
                          'Integrator "volpath"', '"homogeneous"',
                          '"imagemap"', '"point2 uv"', '"integer xresolution" [32]'):
                self.assertIn(token, text)
            self.assertTrue((path.parent / 'pattern.png').read_bytes().startswith(b'\x89PNG'))
            args = command('binary', path, 'out.png', 'out.chk', 5, 'in.chk')
            self.assertEqual(args[-2:], ['--resume', 'in.chk'])
            self.assertIn('--batch-size', args)

    def test_runner_real_signal_on_flushed_readiness_and_expected_exit(self):
        with tempfile.TemporaryDirectory() as temporary:
            for sig in (signal.SIGINT, signal.SIGTERM):
                script = ('import signal,sys\n'
                          f'signal.signal({int(sig)}, lambda *args: sys.exit({128 + sig}))\n'
                          f'print("Checkpoint saved: path (1/{TARGET} samples per pixel)", flush=True)\n'
                          'signal.pause()\n')
                log = Path(temporary) / f'{sig}.log'
                self.assertEqual(run([sys.executable, '-c', script], log, timeout=5,
                                     cancel_signal=sig), 1)
                self.assertIn('Checkpoint saved:', log.read_text())
            with self.assertRaisesRegex(ValueError, 'exit'):
                run([sys.executable, '-c', 'raise SystemExit(2)'], Path(temporary) / 'bad.log')
            with self.assertRaisesRegex(ValueError, 'timed out'):
                run([sys.executable, '-c', 'import signal; signal.pause()'],
                    Path(temporary) / 'timeout.log', timeout=.2)

    def test_runner_progress_readiness_requires_committed_nonzero_batch(self):
        with tempfile.TemporaryDirectory() as temporary:
            script = ('import signal,sys\n'
                      'signal.signal(signal.SIGINT, lambda *args: sys.exit(130))\n'
                      f'print("Committed samples per pixel: 1/{TARGET}", flush=True)\n'
                      'signal.pause()\n')
            script = script.replace(f'print("Committed samples',
                                    f'print("Checkpoint saved: path (0/{TARGET} samples per pixel)", flush=True)\n'
                                    'print("Committed samples')
            self.assertEqual(run([sys.executable, '-c', script], Path(temporary) / 'progress.log',
                                 timeout=5, cancel_signal=signal.SIGINT), 1)
            script = script.replace('pixel: 1/', 'pixel: 0/')
            with self.assertRaisesRegex(ValueError, 'readiness'):
                run([sys.executable, '-c', script], Path(temporary) / 'zero.log',
                    timeout=5, cancel_signal=signal.SIGINT)


if __name__ == '__main__':
    unittest.main()
