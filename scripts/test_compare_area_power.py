#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Area power fixture closure, fixed statistical protocol and scene-only samples."""
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import math
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import compare_area_power as suite
from compare_pbrt import stage_dependencies
from generate_area_power_samples import generate


class AreaPowerComparisonTests(unittest.TestCase):
    def test_fixture_hashes_closure_and_provenance(self):
        provenance = json.loads((suite.FIXTURES/'provenance.json').read_text())
        closure = json.loads((suite.FIXTURES/'dependencies.json').read_text())
        self.assertEqual(provenance['pbrt_revision'], suite.REVISION)
        self.assertEqual(set(provenance['scenes']), set(suite.SCENES))
        for name in suite.SCENES:
            source = suite.staged_source(name)
            for token in (b'"integer pixelsamples" [4096]', b'"integer seed" [0]',
                          b'"integer xresolution" [8]', b'"integer yresolution" [8]',
                          b'"string sensor" "cie1931"', b'Camera "perspective"'):
                self.assertIn(token, source)
            paths = provenance['scenes'][name]['dependencies']
            self.assertEqual(paths, [r['path'] for r in closure[name]])
            filenames = re.findall(rb'"string filename" \["([^"\n]+)"\]', source)
            self.assertTrue(set(p.decode() for p in filenames).issubset(paths))
            with tempfile.TemporaryDirectory() as tmp:
                stage_dependencies(suite.FIXTURES/(name+'.pbrt'), Path(tmp))
                for record in closure[name]:
                    payload = (Path(tmp)/record['path']).read_bytes()
                    self.assertEqual(len(payload), record['bytes'])
                    self.assertEqual(hashlib.sha256(payload).hexdigest(), record['sha256'])
        for record in provenance['owned_assets']:
            self.assertEqual(hashlib.sha256((suite.FIXTURES/record['path']).read_bytes()).hexdigest(), record['sha256'])
        external = provenance['external_assets'][0]
        self.assertEqual(external['license'], 'CC BY 2.0')
        payload = (suite.FIXTURES/external['path']).read_bytes()
        self.assertEqual(hashlib.sha256(payload).hexdigest(), external['sha256'])
        with self.assertRaises(ValueError):
            suite.staged_source('../outside')

    def test_variants_preserve_scene(self):
        self.assertEqual(len(suite.CASES), 14)
        for case in suite.CASES:
            name, source = suite.case_source(case)
            base = suite.staged_source(name)
            self.assertEqual(base.split(b'WorldBegin')[1], source.split(b'WorldBegin')[1])
            self.assertIn(b'PixelFilter "box"', source)
            if case.endswith('sobol-fastowen'):
                self.assertIn(b'Sampler "sobol" "string randomization" ["fastowen"]', source)
            else:
                self.assertIn(b'Sampler "independent"', source)
        with self.assertRaises(ValueError):
            suite.case_source('invalid')

    def test_fixed_seeds_and_case_directories(self):
        seeds = {'rypath': [], 'pbrt': []}
        def render(backend, binary, variant, source):
            seeds[backend].append(int(re.search(rb'"integer seed" \[(\d+)\]', source)[1]))
            self.assertEqual(variant.parent.parent.name, 'hdr-image-sobol-fastowen')
            return [(1., 2., 3.)]*64
        name, source = suite.case_source('hdr-image-sobol-fastowen')
        with tempfile.TemporaryDirectory() as tmp, patch.object(suite, 'render_pfm', side_effect=render):
            result = suite.render_backend_sources(Path('/fake/ours'), Path('/fake/pbrt'),
                suite.FIXTURES/(name+'.pbrt'), Path(tmp), {'rypath': source, 'pbrt': source},
                case='hdr-image-sobol-fastowen')
        self.assertEqual(seeds, {'rypath': list(range(16)), 'pbrt': list(range(16, 32))})
        self.assertEqual(len(suite.statistics_record(result)), 15)

    def test_retained_process_and_diagnostics(self):
        def runner(command, **kwargs):
            output = Path(command[command.index('--outfile')+1])
            output.write_bytes(b'PF\n8 8\n-1\n'+struct.pack('<192f', *([1.]*192)))
            return subprocess.CompletedProcess(command, 0, 'retained output', '')
        with tempfile.TemporaryDirectory() as tmp:
            variant = Path(tmp)/'case.pbrt'
            suite.render_pfm('pbrt', Path('/fake/pbrt'), variant, suite.staged_source('hdr-image'), runner=runner)
            record = json.loads(variant.with_suffix('.process.json').read_text())
            self.assertEqual(record['command'][1:5], ['--render-coord-sys', 'world', '--nthreads', '2'])
            self.assertEqual(variant.with_suffix('.stdout.log').read_text(), 'retained output')
            with self.assertRaisesRegex(ValueError, 'renderer reported ERROR'):
                suite.render_pfm('pbrt', Path('/fake/pbrt'), variant, b'',
                    runner=lambda cmd, **kwargs: subprocess.CompletedProcess(cmd, 0, '', 'ERROR invalid'))

    def test_predeclared_gate_detects_bias_and_rejects_invalid_means(self):
        data = {'rypath': {'full': [(1., 2., 3.)]*16}, 'pbrt': {'full': [(1., 2., 3.)]*16}}
        self.assertTrue(all(r['passed'] for r in suite.statistics_record(data)))
        for factor in (.5, 2):
            biased = copy.deepcopy(data)
            biased['rypath']['full'] = [(factor, 2*factor, 3*factor)]*16
            self.assertTrue(all(not r['passed'] for r in suite.statistics_record(biased)))
        for values in ([(float('nan'), 2, 3)]*16, [(1, 2)]*16, [(1, 2, 3)]*15):
            invalid = copy.deepcopy(data)
            invalid['rypath']['full'] = values
            with self.assertRaises(ValueError):
                suite.statistics_record(invalid)

    def test_black_backend_is_not_a_valid_comparison(self):
        for black_backends in (('rypath',), ('pbrt',), ('rypath', 'pbrt')):
            data = {'rypath': {'full': [(1., 2., 3.)]*16},
                    'pbrt': {'full': [(1., 2., 3.)]*16}}
            for backend in black_backends:
                data[backend]['full'] = [(0., 0., 0.)]*16
            with self.assertRaisesRegex(ValueError, 'no positive signal'):
                suite.statistics_record(data)

    def test_fixture_targets_independent_normalization_contracts(self):
        unequal=suite.staged_source('unequal-triangles')
        self.assertIn(b'"integer indices" [0 1 2 1 3 2]',unequal)
        # Shared mesh declaration: primitive areas are .625 and 1.25, not mesh total.
        self.assertIn(b'.25 -.5 1 1.5 -.5 1 .25 .5 1 1.5 1.5 1',unequal)
        self.assertAlmostEqual((80/(math.pi*.625))/(80/(math.pi*1.25)),2)
        affine=suite.staged_source('affine-two-sided')
        self.assertIn(b'Scale 1.3 .7 1',affine)
        self.assertIn(b'"bool twosided" [true]',affine)
        self.assertIn(b'"spectrum L" "canon_eos_5d_b"',affine)
        hdr=(suite.FIXTURES/'assets/asymmetric.hdr').read_bytes()
        payload=hdr.split(b'-Y 2 +X 4\n')[1]
        rgb=[tuple(payload[i+c]*2.**(payload[i+3]-136) for c in range(3))
             for i in range(0,len(payload),4)]
        self.assertEqual(len(rgb),8)
        self.assertEqual(rgb[0],(8.,1.,.25))
        self.assertGreater(max(max(pixel) for pixel in rgb),1)
        luminance=sum(.2126*r+.7152*g+.0722*b for r,g,b in rgb)/len(rgb)
        self.assertAlmostEqual(luminance,1.22839375)
        self.assertIn(b'"rgb L" [.2 .8 1.4]',suite.staged_source('mixed-lighting'))
        self.assertIn(b'"blackbody L" [4500]',suite.staged_source('curved-patch'))

    def test_reference_pin_and_clean_checkout(self):
        binary=Path('/cache/build/pbrt')
        with patch.object(suite.subprocess,'check_output',side_effect=[suite.REVISION+'\n','']):
            self.assertEqual(suite.verify_reference(binary),suite.REVISION)
        with patch.object(suite.subprocess,'check_output',return_value='wrong revision'):
            with self.assertRaisesRegex(ValueError,'expected PBRT'):
                suite.verify_reference(binary)
        with patch.object(suite.subprocess,'check_output',side_effect=[suite.REVISION,' M src/pbrt/lights.cpp']):
            with self.assertRaisesRegex(ValueError,'must be clean'):
                suite.verify_reference(binary)

    def test_fixture_and_dependency_tampering(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            import shutil
            shutil.copytree(suite.FIXTURES, root/'fixtures')
            fixture=root/'fixtures'
            with patch.object(suite, 'FIXTURES', fixture):
                (fixture/'hdr-image.pbrt').write_bytes(b'tampered')
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    suite.staged_source('hdr-image')
            asset=fixture/'assets/asymmetric.hdr'
            asset.write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                stage_dependencies(fixture/'hdr-image.pbrt', root/'staged')
            asset.unlink()
            with self.assertRaises(FileNotFoundError):
                stage_dependencies(fixture/'hdr-image.pbrt', root/'staged')

    def test_process_failures_and_invalid_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            variant=Path(tmp)/'bad.pbrt'
            with self.assertRaises(subprocess.CalledProcessError):
                suite.render_pfm('pbrt', Path('/fake/pbrt'), variant, b'',
                    runner=lambda cmd, **kw: subprocess.CompletedProcess(cmd, 3, '', 'failed'))
            with self.assertRaises(FileNotFoundError):
                suite.render_pfm('pbrt', Path('/fake/pbrt'), variant, b'',
                    runner=lambda cmd, **kw: subprocess.CompletedProcess(cmd, 0, '', ''))
            def invalid(cmd, **kw):
                Path(cmd[cmd.index('--outfile')+1]).write_bytes(
                    b'PF\n8 8\n-1\n'+struct.pack('<192f', *([float('nan')]*192)))
                return subprocess.CompletedProcess(cmd,0,'','')
            with self.assertRaises(ValueError):
                suite.render_pfm('pbrt',Path('/fake/pbrt'),variant,b'',runner=invalid)

    def test_negative_signal_and_missing_backend_fail(self):
        for data in ({'rypath': {'full': [(1.,2.,3.)]*16}},
            {'rypath': {'full': [(-1.,-1.,-1.)]*16}, 'pbrt': {'full': [(1.,1.,1.)]*16}}):
            with self.assertRaises(ValueError):
                suite.statistics_record(data)

    def test_showcase_has_equal_triangle_counts_and_area_ratio(self):
        with tempfile.TemporaryDirectory() as tmp:
            destination=Path(tmp)
            generate(destination)
            radiance=(destination/'constant-radiance.pbrt').read_text()
            power=(destination/'equal-primitive-power.pbrt').read_text()
            self.assertEqual(power.replace('"float power" [1.00530965]', ''),radiance)
            self.assertEqual(power.count('AreaLightSource'),2)
            self.assertEqual(power.count('"float power" [1.00530965]'),2)
            for token in ('"integer xresolution" [768]', '"integer yresolution" [512]',
                          '"integer pixelsamples" [512]'):
                self.assertIn(token,power)
            self.assertNotIn('LightSource "infinite"',power)
            generate(destination)
            self.assertEqual((destination/'equal-primitive-power.pbrt').read_text(),power)


if __name__ == '__main__':
    unittest.main()
