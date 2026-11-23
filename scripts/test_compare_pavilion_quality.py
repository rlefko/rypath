#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import compare_pavilion_quality as quality


class QualityTests(unittest.TestCase):
    def test_frozen_candidate_uses_build_source(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory).resolve() / 'rypath'
            library = binary.with_name('rypath.metallib')
            binary.write_bytes(b'frozen executable')
            library.write_bytes(b'frozen shaders')
            files = {'src/render.cpp': 'a' * 64}
            source = {'revision': 'b' * 40, 'files': files,
                      'source_sha256': hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()}
            receipt = {'source': source, 'files': {
                str(p): {'sha256': quality.sha(p), 'bytes': p.stat().st_size}
                for p in (binary, library)}}
            path = binary.with_name('artifacts.json')
            quality.write_json(path, receipt)
            # A new checkout revision must not relabel already-built artifacts.
            with patch.object(quality, 'renderer_source_identity', side_effect=AssertionError):
                identity = quality.binary_identity('candidate', binary, {})
                for key, value in source.items():
                    self.assertEqual(identity[key], value)
                receipt['source']['source_sha256'] = '0' * 64
                quality.write_json(path, receipt)
                with self.assertRaisesRegex(ValueError, 'source digest'):
                    quality.binary_identity('candidate', binary, {})
                receipt['source']['source_sha256'] = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
                quality.write_json(path, receipt)
                self.assertEqual(quality.binary_identity('candidate', binary, {}), identity)
                library.write_bytes(b'changed shaders')
                with self.assertRaisesRegex(ValueError, 'artifact hash'):
                    quality.binary_identity('candidate', binary, {})

    def test_live_candidate_without_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory).resolve() / 'rypath'
            library = binary.with_name('rypath.metallib')
            binary.write_bytes(b'executable'); library.write_bytes(b'shaders')
            with patch.object(quality, 'renderer_source_identity', return_value={'source_sha256': 'live'}) as current:
                self.assertEqual(quality.binary_identity('candidate', binary, {})['source_sha256'], 'live')
                current.assert_called_once_with()

    def test_frozen_protocol(self):
        p = quality.load_protocol()
        self.assertEqual(p['primary_gate_count'], 48)
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'protocol.json'
            target.write_text(json.dumps(dict(p, replicates=31)))
            with self.assertRaisesRegex(ValueError, 'protocol hash'):
                quality.load_protocol(target)

    def test_film_only_raw_companion(self):
        source = (b'Film "rgb" "float iso" [500] "float maxcomponentvalue" 50\n'
                  b'"integer xresolution" [1600] "integer yresolution" [850]\n'
                  b'Camera "perspective" "float lensradius" [.0125]\n'
                  b'Sampler "halton" "integer pixelsamples" [256]\n'
                  b'WorldBegin\nInclude "../landscape/tree.pbrt"\n')
        args = dict(scene='day', spp=64, seed=33, output=Path('/tmp/out.exr'))
        clamped = quality.variant_source(source, **args)
        raw = quality.variant_source(source, raw=True, **args)
        self.assertIn(b'"float iso" [500]', raw)
        self.assertNotIn(b'maxcomponentvalue', raw)
        self.assertIn(b'maxcomponentvalue', clamped)
        self.assertIn(b'"integer seed" [33]', raw)
        self.assertIn(b'"bool savefp16" [false]', raw)
        self.assertEqual(raw.split(b'WorldBegin')[1], source.split(b'WorldBegin')[1])
        self.assertEqual(quality.parameter(clamped, 'float', 'maxcomponentvalue', '', remove=True), raw)

    def test_record_reuse_hashes_and_exposure(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            raw, clamp = directory / 'raw.exr', directory / 'clamp.exr'
            raw.write_bytes(b'raw'); clamp.write_bytes(b'clamp')
            record = {'status': 'complete', 'protocol_sha256': quality.PROTOCOL_SHA256,
                      'backend': 'candidate', 'scene': 'day', 'sampler_seed': 4, 'variants': [{'path': str(raw), 'sha256': quality.sha(raw)}],
                      'artifacts': {
                          key: {'path': str(path), 'sha256': quality.sha(path), 'scale': 1}
                          for key, path in [('raw', raw), ('clamped', clamp)]}}
            path = directory / 'record.json'
            quality.write_json(path, record)
            self.assertEqual(quality.validate_record(path, {'sampler_seed': 4}), record)
            with self.assertRaisesRegex(ValueError, 'reuse identity'):
                quality.validate_record(path, {'sampler_seed': 5})
            raw.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'artifact hash'):
                quality.validate_record(path)
            record['artifacts']['raw']['sha256'] = quality.sha(raw)
            record['artifacts']['raw']['scale'] = float('nan')
            path.write_text(json.dumps(record))
            with self.assertRaisesRegex(ValueError, 'nonfinite'):
                quality.validate_record(path)

    def test_exact_frozen_exposure_scales(self):
        protocol = quality.load_protocol()
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            artifact = directory / 'image.exr'
            artifact.write_bytes(b'intact artifact')
            digest = quality.sha(artifact)
            path = directory / 'record.json'
            for backend in quality.BACKENDS:
                for scene in ('day', 'night'):
                    scales = {'raw': 100 / protocol['scenes'][scene]['iso']
                              if backend == 'pbrt' else 1, 'clamped': 1}
                    record = {'backend': backend, 'scene': scene, 'status': 'complete',
                              'protocol_sha256': quality.PROTOCOL_SHA256,
                              'variants': [{'path': str(artifact), 'sha256': digest,
                                            'transport_sha256': 'same transport'}],
                              'artifacts': {plane: {'path': str(artifact), 'sha256': digest,
                                                    'scale': scale}
                                            for plane, scale in scales.items()}}
                    quality.write_json(path, record)
                    self.assertEqual(quality.validate_record(path), record)
                    for plane in ('raw', 'clamped'):
                        with self.subTest(backend=backend, scene=scene, plane=plane):
                            # Original hashes remain intact: only a finite exposure is wrong.
                            record['artifacts'][plane]['scale'] = 1 if scales[plane] != 1 else .2
                            quality.write_json(path, record)
                            with self.assertRaisesRegex(ValueError, 'exposure.*frozen protocol'):
                                quality.validate_record(path)
                            record['artifacts'][plane]['scale'] = scales[plane]


    def test_missing_planes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'record.json'
            quality.write_json(path, {'status': 'complete', 'protocol_sha256': quality.PROTOCOL_SHA256,
                                     'artifacts': {}})
            with self.assertRaisesRegex(ValueError, 'missing retained planes'):
                quality.validate_record(path)

    def test_dependency_mismatch(self):
        item = {'path': 'barcelona-pavilion/asset', 'bytes': 1, 'git_blob_sha1': '0' * 40}
        with tempfile.TemporaryDirectory() as directory:
            asset = Path(directory) / item['path']
            asset.parent.mkdir(parents=True)
            asset.write_bytes(b'x')
            with self.assertRaisesRegex(ValueError, 'dependency hash'):
                quality.verify_closure(Path(directory), {'files': [item] * 159})

    def test_missing_replicas_and_no_partial_acceptance(self):
        args = quality.parse_args(['analyze', '--scene', 'day'])
        with tempfile.TemporaryDirectory() as directory:
            args.output_dir = Path(directory)
            with patch.object(quality, 'binary_identity', return_value={}), \
                    patch.object(quality, 'verify_closure'):
                with self.assertRaisesRegex(ValueError, 'missing required replica'):
                    quality.analyze(args, quality.load_protocol())

    def test_failed_and_inconclusive_bounds(self):
        value = {'bootstrap_draws': 20000, 'simultaneous_log_margin': 0,
                 'gates': [{'dataset': 'day/raw', 'region': 'full', 'metric': 'variance',
                            'candidate': 2, 'reference': 1, 'epsilon': 1e-12,
                            'log_upper': .7, 'upper': 2, 'absolute': False, 'pass': False}],
                 'mean_gates': [], 'gate_count': 1, 'complete': False, 'accepted': False}
        self.assertFalse(quality.validate_analysis_result(value, quality.load_protocol()))
        value['gates'][0]['pass'] = True
        with self.assertRaisesRegex(ValueError, 'failure bound'):
            quality.validate_analysis_result(value, quality.load_protocol())
        value['gates'][0]['pass'] = False
        value['simultaneous_log_margin'] = float('nan')
        with self.assertRaisesRegex(ValueError, 'inconclusive'):
            quality.validate_analysis_result(value, quality.load_protocol())


    def test_selections_preserve_frozen_seeds(self):
        args = quality.parse_args(['render', '--scene', 'night', '--spp', '64', '--backend', 'pbrt',
                                   '--replicate-start', '1', '--replicate-end', '3'])
        self.assertEqual(list(quality.selected(quality.load_protocol(), args)),
                         [('pbrt', 'night', 64, 33), ('pbrt', 'night', 64, 34)])
        args.spp = [256]
        with self.assertRaisesRegex(ValueError, 'budget'):
            list(quality.selected(quality.load_protocol(), args))


if __name__ == '__main__':
    unittest.main()
