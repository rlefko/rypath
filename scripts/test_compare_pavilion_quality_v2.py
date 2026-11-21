#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic contracts only. No renderer, metrics executable or real EXR launch."""
import copy
import hashlib
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
import types
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_pavilion_quality_v2 as q
import benchmark_pavilion_quality_v2 as timing


def frozen(directory):
    value = json.loads(q.PROTOCOL.read_text())
    evidence = directory / 'observed-evidence.json'
    evidence.write_text(json.dumps({'candidate': [0, 1], 'reference': [32, 33]}))
    ledger = directory / 'seed-ledger.json'
    ledger.write_text(json.dumps({'inventory_complete': True,
                                 'observed_ids': {'candidate': [0, 1], 'reference': [32, 33]},
                                 'bindings': {str(evidence): q.sha(evidence)}}))
    value.update(status='frozen-before-outcomes',
                 seed_ids={'candidate': list(range(10000, 10064)),
                           'reference': list(range(20000, 20064))},
                 seed_usage_ledger={'path': str(ledger), 'sha256': q.sha(ledger)},
                 freeze={'utc': '2026-11-21T00:00:00+00:00', 'before_outcomes': True},
                 contracts={'candidate': [3, 12, 21, 7, 2], 'reference': 'modified-pbrt-v4'})
    model_hashes = {'candidate': 'a' * 64, 'reference': 'b' * 64}
    review = directory / 'model-review.json'
    review.write_text(json.dumps({'status': 'approved-matched-target', 'target_id': 'fixture-model',
                                  'arm_model_sha256': model_hashes}))
    value['models'] = {'target_id': 'fixture-model',
                       'review': {'path': str(review), 'sha256': q.sha(review)},
                       **{arm: {'target_id': 'fixture-model', 'model_sha256': h}
                          for arm, h in model_hashes.items()}}
    value['metrics_engine'] = {'kind': 'pavilion-quality-v2-two-arm',
                               'binary': str(directory / 'metrics'), 'sha256': 'e' * 64,
                               'source_sha256': 'f' * 64, 'numerical_core_sha256': '1' * 64}
    value['artifacts'] = {}
    for arm in q.ARMS:
        binary = directory / arm
        binary.write_bytes(b'not executable test data')
        source = directory / (arm + '-source.json')
        source.write_text(json.dumps({'source': 'synthetic fixture'}))
        value['artifacts'][arm] = {'kind': 'rypath' if arm == 'candidate'
                                         else 'modified-pbrt-diagnostic',
                                   'stock_verified': False, 'binary': str(binary),
                                   'binary_sha256': q.sha(binary),
                                   'source_manifest': str(source),
                                   'source_manifest_sha256': q.sha(source)}
    return value


def native_result(protocol):
    gates = []
    for scene in protocol['scenes']:
        for plane in protocol['planes']:
            for region in protocol['regions']:
                for metric in ('variance', 'tail'):
                    gates.append({'dataset': scene + '/' + plane, 'region': region,
                                  'metric': metric, 'candidate': 1., 'reference': 1.,
                                  'epsilon': 1e-12, 'absolute': False, 'log_upper': 0.,
                                  'upper': 1., 'pass': True})
    means = [{'dataset': scene + '/raw', 'region': region, 'channel': channel,
              'candidate': 1., 'reference': 1., 'difference': 0., 'combined_se': 0.,
              'threshold': 2e-5, 'pass': True}
             for scene in protocol['scenes'] for region in protocol['regions'] for channel in range(3)]
    return {'engine': 'pavilion-quality-v2-two-arm', 'replicates': 64, 'backend_count': 2,
            'resolution': [320, 170], 'bootstrap_draws': 20000,
            'bootstrap_seed': protocol['bootstrap']['seed'],
            'bootstrap_rng': 'mt19937-per-draw-seed-seq-rejection',
            'simultaneous_log_margin': 0., 'gates': gates, 'gate_count': 48,
            'mean_gates': means, 'complete': True, 'accepted': True}


def reference_record(directory, protocol):
    arm, scene, seed = 'reference', 'night', protocol['seed_ids']['reference'][0]
    image = {'resolution': [320, 170], 'data_window': [[0, 0], [319, 169]],
             'display_window': [[0, 0], [319, 169]], 'channels': ['R', 'G', 'B'], 'storage': 'FLOAT'}
    record = {'kind': 'quality-v2-replicate', 'status': 'complete', 'protocol_sha256': 'c' * 64,
              'arm': arm, 'scene': scene, 'spp': 512, 'sampler_seed': seed, 'material_seed': seed,
              'model': protocol['models'][arm], 'target_id': protocol['models']['target_id'],
              'renderer': protocol['artifacts'][arm], 'contract': protocol['contracts'][arm],
              'started_utc': '2026-11-22T00:00:00+00:00', 'artifacts': {}, 'variants': [],
              'processes': []}
    source = directory / 'pavilion-night.pbrt'
    source.write_bytes(b'Film "rgb" "float iso" [250] "float maxcomponentvalue" [50]\n'
                       b'Sampler "halton" "integer pixelsamples" [512] "integer seed" [0]\n'
                       b'WorldBegin\n')
    record['original_scene'] = {'path': str(source), 'sha256': q.sha(source)}
    for plane in ('clamped', 'raw'):
        path = directory / (plane + '.exr')
        path.write_bytes(b'synthetic image bytes; reader is explicitly injected')
        record['artifacts'][plane] = {'path': str(path), 'sha256': q.sha(path),
                                       'scale': q.required_scale(arm, scene, plane, protocol),
                                       'image': copy.deepcopy(image)}
        variant = directory / (plane + '.pbrt')
        variant.write_bytes(q.inherited.variant_source(source.read_bytes(), scene=scene,
                            spp=512, seed=seed, output=path, raw=plane == 'raw'))
        record['variants'].append({'path': str(variant), 'sha256': q.sha(variant),
                                    'transport_sha256': hashlib.sha256(q.inherited.replace_descriptor(
                                        variant.read_bytes(), 'Film', b'Film')[0]).hexdigest(),
                                    'plane': plane})
        log = directory / (plane + '.log')
        log.write_text('synthetic successful process\n')
        record['processes'].append({'exit_code': 0, 'status': 'exited', 'log': str(log),
                                     'log_sha256': q.sha(log),
                                     'command': q.renderer_command(protocol, arm, seed, variant,
                                                                   path, directory)})
    return record, lambda _: copy.deepcopy(image)


class ProtocolTests(unittest.TestCase):
    def test_draft_cannot_run(self):
        value = json.loads(q.PROTOCOL.read_text())
        q.validate_protocol(value, allow_draft=True)
        with self.assertRaisesRegex(ValueError, 'draft'):
            q.validate_protocol(value)

    def test_two_real_arms_and64fresh_ids(self):
        with tempfile.TemporaryDirectory() as name:
            p = frozen(Path(name))
            q.validate_protocol(p)
            self.assertEqual(len(q.quality_inventory(p)), 256)
            self.assertNotIn('baseline', p['seed_ids'])
            for change in ('duplicate', 'missing', 'reused', 'cross-arm', 'model', 'stock'):
                v = copy.deepcopy(p)
                if change == 'duplicate': v['seed_ids']['candidate'][1] = v['seed_ids']['candidate'][0]
                if change == 'missing': v['seed_ids']['candidate'].pop()
                if change == 'reused': v['seed_ids']['candidate'][0] = 1
                if change == 'cross-arm': v['seed_ids']['reference'][0] = v['seed_ids']['candidate'][0]
                if change == 'model': v['models']['reference']['model_sha256'] = 'f' * 64
                if change == 'stock': v['reference_policy']['stock_verified'] = True
                with self.subTest(change=change), self.assertRaises(ValueError):
                    q.validate_protocol(v)

    def test_candidate_library_must_be_actual_resolved_sibling(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            protocol = frozen(directory)
            item = protocol['artifacts']['candidate']
            binary = Path(item['binary'])
            binary.chmod(0o755)
            actual = binary.with_name('rypath.metallib')
            actual.write_bytes(b'actual loaded library')
            item.update(metallib=str(actual), metallib_sha256=q.sha(actual))
            q.artifact_identity(protocol, 'candidate')
            elsewhere = directory / 'unloaded'
            elsewhere.mkdir()
            unrelated = elsewhere / 'rypath.metallib'
            unrelated.write_bytes(actual.read_bytes())
            item['metallib'] = str(unrelated)
            with self.assertRaisesRegex(ValueError, 'resolved executable sibling'):
                q.artifact_identity(protocol, 'candidate')
            item['metallib'] = str(actual)
            actual.write_bytes(b'changed actual sibling')
            with self.assertRaisesRegex(ValueError, 'metallib changed'):
                q.artifact_identity(protocol, 'candidate')

    def test_executable_symlink_uses_target_parent_library(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            protocol = frozen(directory)
            item = protocol['artifacts']['candidate']
            target = Path(item['binary'])
            target.chmod(0o755)
            actual = target.with_name('rypath.metallib')
            actual.write_bytes(b'target library')
            links = directory / 'launcher'
            links.mkdir()
            executable = links / 'rypath'
            executable.symlink_to(target)
            item.update(binary=str(executable), metallib=str(actual),
                        metallib_sha256=q.sha(actual))
            q.artifact_identity(protocol, 'candidate')
            wrong = links / 'rypath.metallib'
            wrong.write_bytes(actual.read_bytes())
            item['metallib'] = str(wrong)
            with self.assertRaisesRegex(ValueError, 'resolved executable sibling'):
                q.artifact_identity(protocol, 'candidate')

    def test_reject_modified_numerical_policy(self):
        with tempfile.TemporaryDirectory() as name:
            for key, value in [('maximum_ratio', 1.3), ('replicates', 32),
                               ('primary_gate_count', 47)]:
                p = frozen(Path(name)); p[key] = value
                with self.subTest(key=key), self.assertRaises(ValueError):
                    q.validate_protocol(p)

    def test_record_model_scale_window_contract_and_planes(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name); p = frozen(directory)
            r, reader = reference_record(directory, p)
            q.validate_record(r, p, 'c' * 64, image_reader=reader)
            for change in ('model', 'scale', 'window', 'contract', 'plane', 'seed', 'pre-freeze'):
                v = copy.deepcopy(r)
                if change == 'model': v['target_id'] = 'different-model'
                if change == 'scale': v['artifacts']['raw']['scale'] = 1
                if change == 'window': v['artifacts']['raw']['image']['data_window'][0] = [1, 0]
                if change == 'contract': v['contract'] = [3, 12, 19, 7, 2]
                if change == 'plane': del v['artifacts']['raw']
                if change == 'seed': v['sampler_seed'] = 32
                if change == 'pre-freeze': v['started_utc'] = '2025-01-01T00:00:00+00:00'
                with self.subTest(change=change), self.assertRaises(ValueError):
                    q.validate_record(v, p, 'c' * 64, image_reader=reader)

    def test_missing_record_inventory_fails(self):
        with tempfile.TemporaryDirectory() as name:
            p = frozen(Path(name))
            with self.assertRaisesRegex(ValueError, 'missing required replicate'):
                q.collect_records(Path(name) / 'empty', p, 'c' * 64)

    def test_duplicate_record_slot_and_extra_inventory(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            protocol = frozen(directory)
            root = directory / 'inventory'
            for arm, scene, spp, seed in q.quality_inventory(protocol):
                path = q.record_path(root, arm, scene, spp, seed)
                path.parent.mkdir(parents=True)
                path.write_text(json.dumps({'arm': arm, 'scene': scene, 'spp': spp,
                                            'sampler_seed': seed}))
            # This unit isolates inventory addressing. Full record semantics are
            # checked separately with retained-source/log/scale/model fixtures.
            with patch.object(q, 'validate_record', side_effect=lambda raw, *_a, **_k: raw):
                self.assertEqual(len(q.collect_records(root, protocol, 'c' * 64)), 256)
                target = q.record_path(root, 'candidate', 'day', 256, 10000)
                raw = json.loads(target.read_text())
                raw['sampler_seed'] = 10001
                target.write_text(json.dumps(raw))
                with self.assertRaisesRegex(ValueError, 'wrong endpoint slot'):
                    q.collect_records(root, protocol, 'c' * 64)
                raw['sampler_seed'] = 10000
                target.write_text(json.dumps(raw))
                extra = root / 'runs' / 'extra' / 'record.json'
                extra.parent.mkdir(parents=True)
                extra.write_text('{}')
                with self.assertRaisesRegex(ValueError, 'extra/partial'):
                    q.collect_records(root, protocol, 'c' * 64)

    def test_actual_reader_rejects_shifted_window_before_pixel_loading(self):
        class Image:
            def __init__(self, *_args, **_kwargs): pass
            def __enter__(self): return self
            def __exit__(self, *_args): return False
            def header(self):
                return {'dataWindow': ([1, 0], [320, 169]),
                        'displayWindow': ([0, 0], [319, 169])}
            def channels(self): return {}
        with patch.dict(sys.modules, {'OpenEXR': types.SimpleNamespace(File=Image),
                                      'numpy': types.SimpleNamespace()}):
            with self.assertRaisesRegex(ValueError, 'wrong image window'):
                q.read_image('synthetic-header-only')

    def test_endpoint_families_and_verdicts(self):
        p = json.loads(q.PROTOCOL.read_text())
        q.validate_analysis_result(native_result(p), p)
        for change in ('missing-metric', 'duplicate', 'missing-mean', 'wrong64', 'third-arm',
                       'nan-margin', 'mean-flag', 'ratio-policy'):
            v = native_result(p)
            if change == 'missing-metric': v['gates'].pop(); v['gate_count'] = 47
            if change == 'duplicate': v['gates'][-1] = v['gates'][0]
            if change == 'missing-mean': v['mean_gates'].pop()
            if change == 'wrong64': v['replicates'] = 32
            if change == 'third-arm': v['backend_count'] = 3
            if change == 'nan-margin': v['simultaneous_log_margin'] = float('nan')
            if change == 'mean-flag': v['mean_gates'][0]['pass'] = False
            if change == 'ratio-policy': v['gates'][0]['upper'] = 1.3
            with self.subTest(change=change), self.assertRaises(ValueError):
                q.validate_analysis_result(v, p)

    def test_epsilon_branch_cannot_use_absolute1e12_tolerance(self):
        p = json.loads(q.PROTOCOL.read_text()); v = native_result(p)
        g = v['gates'][0]
        g.update(candidate=2e-12, reference=0., epsilon=1e-12, absolute=True,
                 log_upper=math.log(2e-12) - math.log(1e-12), upper=2e-12, **{'pass': False})
        v['accepted'] = False
        self.assertFalse(q.validate_analysis_result(v, p))
        g.update(upper=0., **{'pass': True}); v['accepted'] = True
        with self.assertRaisesRegex(ValueError, 'upper bound inconsistent'):
            q.validate_analysis_result(v, p)

    def test_timing_cannot_be_quality_replicas(self):
        p = json.loads(q.PROTOCOL.read_text())
        rows = list(timing.order(p))
        self.assertEqual(len(rows), 48)
        self.assertEqual(sum(row[3] == 'warmup' for row in rows), 8)
        self.assertEqual({row[0] for row in rows}, set(q.ARMS))
        day64 = [row for row in rows if row[1:3] == ('day', 64)]
        self.assertEqual([row[0] for row in day64],
                         ['candidate', 'reference', 'candidate', 'reference',
                          'reference', 'candidate', 'candidate', 'reference',
                          'reference', 'candidate', 'candidate', 'reference'])
        changed = json.loads(q.PROTOCOL.read_text())
        changed['timing']['repetitions'] = 3
        with self.assertRaises(ValueError):
            list(timing.order(changed))
        with tempfile.TemporaryDirectory() as name:
            protocol = frozen(Path(name)); record, reader = reference_record(Path(name), protocol)
            record['kind'] = 'quality-v2-timing-only'
            with self.assertRaises(ValueError):
                q.validate_record(record, protocol, 'c' * 64, image_reader=reader)


if __name__ == '__main__':
    unittest.main()
