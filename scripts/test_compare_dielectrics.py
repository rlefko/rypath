#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check explicit, identical dielectric reference variants without a GPU."""
import unittest
import tempfile
import hashlib
import json
from pathlib import Path

from compare_dielectrics import SCENES, comparison_variant, rypath_artifact_hashes
from compare_pbrt import stage_dependencies


class RendererArtifactTests(unittest.TestCase):
    def test_shader_hash_tracks_updates_and_uses_canonical_executable_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            actual = root/'build'
            actual.mkdir()
            executable = actual/'rypath_pbrt_reference'
            executable.write_bytes(b'unchanged host executable')
            library = actual/'rypath.metallib'
            library.write_bytes(b'original shader')
            alias = root/'reference-link'
            alias.symlink_to(executable)
            (root/'rypath.metallib').write_bytes(b'wrong library beside alias')
            before = rypath_artifact_hashes(alias)
            self.assertEqual(before, {
                str(executable.resolve()): hashlib.sha256(executable.read_bytes()).hexdigest(),
                str(library.resolve()): hashlib.sha256(library.read_bytes()).hexdigest(),
            })
            library.write_bytes(b'grazing shader fix')
            after = rypath_artifact_hashes(alias)
            self.assertEqual(before[str(executable.resolve())], after[str(executable.resolve())])
            self.assertNotEqual(before[str(library.resolve())], after[str(library.resolve())])

    def test_missing_default_library_rejects_incomplete_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory)/'rypath_pbrt_reference'
            executable.write_bytes(b'host executable')
            with self.assertRaises(FileNotFoundError):
                rypath_artifact_hashes(executable)


class VariantTests(unittest.TestCase):
    def test_positive_floor_changes_only_declared_endpoint_and_retains_native_fixture(self):
        path = Path(__file__).resolve().parents[1]/'tests/fixtures/pbrt-render/dielectric/roughness-graph.pbrt'
        original = path.read_bytes()
        variant, overrides = comparison_variant('roughness-graph', original)
        self.assertEqual(variant, original.replace(b'"float tex1" [0]', b'"float tex1" [0.01]', 1))
        self.assertEqual(len(overrides), 1)
        self.assertEqual(overrides[0]['original'], '"float tex1" [0]')
        self.assertEqual(overrides[0]['replacement'], '"float tex1" [0.01]')
        self.assertIn(b'"float tex2" [0.15]', variant)
        self.assertIn(b'"integer pixelsamples" [4096]', variant)
        self.assertIn(b'"integer seed" [0]', variant)
        self.assertEqual(path.read_bytes(), original)

    def test_other_fixtures_are_unchanged_and_missing_or_duplicate_endpoint_fails(self):
        source = b'Material "dielectric" "float roughness" [0]\n'
        self.assertEqual(comparison_variant('smooth-environment', source), (source, []))
        for invalid in (source, b'"float tex1" [0]\n"float tex1" [0]'):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, 'expected one'):
                comparison_variant('roughness-graph', invalid)


class FixtureClosureTests(unittest.TestCase):
    def test_pinned_entrypoints_and_complete_staged_dependencies(self):
        root = Path(__file__).resolve().parents[1]/'tests/fixtures/pbrt-render/dielectric'
        manifest = json.loads((root/'manifest.json').read_text())
        dependencies = json.loads((root/'dependencies.json').read_text())
        self.assertEqual(set(manifest), set(SCENES))
        self.assertEqual(set(dependencies), set(SCENES))
        for scene in SCENES:
            with self.subTest(scene=scene), tempfile.TemporaryDirectory() as directory:
                fixture = root/(scene+'.pbrt')
                payload = fixture.read_bytes()
                self.assertEqual(len(payload), manifest[scene]['bytes'])
                self.assertEqual(hashlib.sha256(payload).hexdigest(), manifest[scene]['sha256'])
                for setting in (b'"integer xresolution" [8]', b'"integer yresolution" [8]',
                                b'"integer pixelsamples" [4096]', b'"integer seed" [0]'):
                    self.assertIn(setting, payload)
                stage_dependencies(fixture, Path(directory))
                for record in dependencies[scene]:
                    staged = (Path(directory)/record['path']).read_bytes()
                    self.assertEqual(len(staged), record['bytes'])
                    self.assertEqual(hashlib.sha256(staged).hexdigest(), record['sha256'])



if __name__ == '__main__':
    unittest.main()
