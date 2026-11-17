#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Small offline acquisition tests; no scene corpus or live network required."""
import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import fetch_pbrt_pavilion as suite


class PavilionAcquisitionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.local = self.root / 'local'
        self.local.mkdir()
        self.payload = b'WorldBegin\n'
        self.record = {'path': 'barcelona-pavilion/scene.pbrt', 'bytes': len(self.payload),
                       'git_blob_sha1': suite.git_blob_sha1(self.payload),
                       'sha256': hashlib.sha256(self.payload).hexdigest()}
        self.manifest = {'repository': 'mmp/pbrt-v4-scenes', 'revision': '0' * 40,
                         'entry_points': [self.record['path']], 'files': [self.record]}
        self.path = self.root / 'manifest.json'
        self.write_manifest()
        self.args = argparse.Namespace(manifest=self.path, cache=self.root / 'cache',
                    local_root=[self.local], local_file=[], fetch=True, max_download_bytes=0)

    def write_manifest(self):
        self.path.write_text(json.dumps(self.manifest))

    def local_asset(self):
        source = self.local / self.record['path']
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(self.payload)
        return source

    def target(self):
        return self.args.cache / self.manifest['revision'] / self.record['path']

    def test_copy_is_verified_snapshot_and_second_run_has_no_download(self):
        source = self.local_asset()
        with patch.object(suite.urllib.request, 'urlopen') as network:
            first = suite.stage(self.args)
            self.assertEqual(first['verified_files'], 1)
            self.assertEqual(first['copy_bytes'], len(self.payload))
            source.write_bytes(b'changed mutable source')
            second = suite.stage(self.args)
            self.assertEqual(second['verified_cache_files'], 1)
            self.assertEqual(second['download_bytes'], 0)
            network.assert_not_called()
        self.assertEqual(self.target().read_bytes(), self.payload)
        receipt = json.loads((self.target().parents[1] / 'rypath-acquisition.json').read_text())
        self.assertEqual(receipt['files'][0]['sha256'], self.record['sha256'])

    def test_corrupt_cache_repaired_from_verified_local_copy(self):
        self.local_asset()
        suite.stage(self.args)
        self.target().write_bytes(b'corrupted')
        with patch.object(suite.urllib.request, 'urlopen') as network:
            result = suite.stage(self.args)
            self.assertEqual(result['copy_bytes'], len(self.payload))
            network.assert_not_called()
        self.assertEqual(self.target().read_bytes(), self.payload)

    def test_read_only_plan_and_budget_reject_before_network(self):
        self.args.fetch = False
        plan = suite.stage(self.args)
        self.assertEqual(plan['download_bytes'], len(self.payload))
        self.assertFalse(self.args.cache.exists())
        self.args.fetch = True
        with patch.object(suite.urllib.request, 'urlopen') as network:
            with self.assertRaisesRegex(ValueError, 'Download plan'):
                suite.stage(self.args)
            network.assert_not_called()
        self.assertFalse(self.args.cache.exists())

    def test_download_verifies_both_hashes_and_size_before_publication(self):
        self.args.max_download_bytes = len(self.payload)
        for payload in (self.payload + b'overflow', b'wrong same!'):
            with self.subTest(payload=payload), patch.object(suite.urllib.request, 'urlopen',
                    return_value=io.BytesIO(payload)) as network:
                with self.assertRaisesRegex(ValueError, 'verification failed'):
                    suite.stage(self.args)
                self.assertEqual(network.call_args.kwargs['timeout'], 45)
                self.assertFalse(self.target().exists())
        self.record['sha256'] = 'f' * 64
        self.write_manifest()
        with patch.object(suite.urllib.request, 'urlopen', return_value=io.BytesIO(self.payload)):
            with self.assertRaisesRegex(ValueError, 'SHA-256'):
                suite.stage(self.args)
        self.record['sha256'] = hashlib.sha256(self.payload).hexdigest()
        self.write_manifest()
        with patch.object(suite.urllib.request, 'urlopen', return_value=io.BytesIO(self.payload)):
            self.assertEqual(suite.stage(self.args)['verified_files'], 1)
        self.assertEqual(self.target().read_bytes(), self.payload)

    def test_manifest_rejects_traversal_duplicates_and_unpinned_revisions(self):
        for name in ('../outside', '/absolute'):
            changed = copy.deepcopy(self.manifest)
            changed['files'][0]['path'] = name
            self.path.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, 'Unsafe'):
                suite.load_manifest(self.path)
        changed = copy.deepcopy(self.manifest)
        changed['files'].append(changed['files'][0])
        self.path.write_text(json.dumps(changed))
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            suite.load_manifest(self.path)
        changed['files'].pop()
        changed['revision'] = 'main'
        self.path.write_text(json.dumps(changed))
        with self.assertRaisesRegex(ValueError, 'full Git revision'):
            suite.load_manifest(self.path)

    def test_symlink_escape_rejected_without_network_or_outside_write(self):
        outside = self.root / 'outside'
        outside.mkdir()
        cache_root = self.args.cache / self.manifest['revision']
        cache_root.mkdir(parents=True)
        (cache_root / 'barcelona-pavilion').symlink_to(outside, target_is_directory=True)
        with patch.object(suite.urllib.request, 'urlopen') as network:
            with self.assertRaisesRegex(ValueError, 'escapes root'):
                suite.stage(self.args)
            network.assert_not_called()
        self.assertEqual(list(outside.iterdir()), [])

    def test_tracked_manifest_has_full_hashes_and_both_entry_points(self):
        manifest = suite.load_manifest(suite.MANIFEST)
        self.assertEqual(len(manifest['files']), 159)
        self.assertEqual(len(manifest['entry_points']), 2)
        self.assertEqual(sum(r['bytes'] for r in manifest['files']), manifest['total_bytes'])
        self.assertTrue(all(len(r['sha256']) == 64 and r['asset_terms']
                            for r in manifest['files']))


if __name__ == '__main__':
    unittest.main()
