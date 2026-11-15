#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

import contextlib
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

from compare_pbrt import (
    EWA_ELLIPSE, FILTER_CONTROLS, FULL, QUADRANTS, REGIONS, TRILINEAR_BLEND,
    check_filter_sensitivity, compare_regions, main, make_filter_variant, read_pfm, roi_means,
    render_pfm, render_replicates, stage_dependencies,
)


CORNELL_CREDIT = (b"# SPDX-License-Identifier: CC0-1.0\n"
                  b"# Cornell Box by Benedikt Bitterli, converted to PBRT v4 by Matt Pharr.\n"
                  b"# Adapted for comparison in rypath. See ../pbrt/README.md and ../pbrt/LICENSE.txt.\n")
CORNELL_FILM_EDIT = (
    b'"integer yresolution" [ 1024 ]\n    "integer xresolution" [ 1024 ]',
    b'"integer yresolution" [8]\n    "integer xresolution" [8]\n'
    b'    "string sensor" "cie1931"\n    "float iso" [100]\n    "bool savefp16" [false]',
)


class SceneSelectionTests(unittest.TestCase):
    def test_named_scenes_keep_fixed_regions_and_stage_image_dependencies(self):
        fixtures = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "pbrt-render"
        scenes = {
            "image16-magnified": ("asymmetric16.png", (16, 2), QUADRANTS),
            "image16-trilinear": (
                "basn2c16.png", (16, 2), {"full": FULL, "blend": TRILINEAR_BLEND}),
            "image16-ewa": ("basn0g16.png", (16, 0), {"full": FULL, "ellipse": EWA_ELLIPSE}),
            "image-indexed-point": (
                "tp1n3p08.png", (8, 3), {"full": FULL, "transparent-texel": (0, 0, 1, 1)}),
            "image-indexed-trilinear": (
                "s35n3p04.png", (4, 3), {"full": FULL, "blend": TRILINEAR_BLEND}),
            "image-grayscale-point": (
                "tbbn0g04.png", (4, 0),
                {"full": FULL, "transparent-texel": (0, 0, 1, 1), "midgray-texel": (6, 4, 7, 5)}),
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            selections = [arg for scene in scenes for arg in ("--scene", scene)]
            # Pass existing paths and mock only renderer execution and result analysis.
            with mock.patch("sys.argv", ["compare_pbrt.py", "--rypath", __file__,
                                         "--pbrt", __file__, "--fixtures", str(fixtures),
                                         "--output-dir", directory, *selections]), \
                    mock.patch("compare_pbrt.render_replicates") as render, \
                    mock.patch("compare_pbrt.compare_regions", return_value=True), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(), 0)
            self.assertEqual(
                [(args[2].stem, args[4]) for args, _ in render.call_args_list],
                [(scene, regions) for scene, (_, _, regions) in scenes.items()],
            )
            for scene, (filename, format_bytes, _) in scenes.items():
                destination = root / scene
                fixture = fixtures / f"{scene}.pbrt"
                stage_dependencies(fixture, destination)
                relative = Path("image-assets") / filename
                staged = (destination / relative).read_bytes()
                self.assertEqual(staged, (fixtures / relative).read_bytes())
                self.assertEqual(tuple(staged[24:26]), format_bytes)
                self.assertIn(str(relative).encode(), fixture.read_bytes())


class PFMTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name) / "test.pfm"

    def test_rgb_range_and_row_order_in_both_byte_orders(self):
        # The two scanlines have distinct values. RGB uses a signed, unbounded range.
        top = (-2, 0.25, 4, 2, 3, 5)
        bottom = (8, -1, 3, 7, 6, 5)
        for scale, order in ((b"-1.0", "<"), (b"1.0", ">")):
            with self.subTest(scale=scale):
                self.path.write_bytes(b"PF\n2 2\n" + scale + b"\n" +
                                      struct.pack(order + "12f", *bottom, *top))
                self.assertEqual(
                    read_pfm(self.path, 2, 2),
                    [top[:3], top[3:], bottom[:3], bottom[3:]],
                )

    def test_rejects_malformed_pfm(self):
        payload = struct.pack("<3f", 0, 1, 2)
        for header, pixels in (
            (b"Pf\n1 1\n-1\n", payload),
            (b"PF\n99999999999999999 1\n-1\n", payload),
            (b"PF\n" + b" " * 100 + b"1 1\n-1\n", payload),
            (b"PF\n1 1\n2\n", payload),
            (b"PF\n1 1\nnan\n", payload),
            (b"PF\n1 1\n-1\n", payload[:-1]),
            (b"PF\n1 1\n-1\n", payload + b"\0"),
            (b"PF\n1 1\n-1\n", struct.pack("<3f", float("inf"), 0, 0)),
        ):
            with self.subTest(header=header, pixels=pixels):
                self.path.write_bytes(header + pixels)
                with self.assertRaises(ValueError):
                    read_pfm(self.path, 1, 1)


class RendererDiagnosticsTests(unittest.TestCase):
    def test_exit_zero_finite_output_with_error_is_rejected_and_retained(self):
        for stream in ("stdout", "stderr"):
            with self.subTest(stream=stream), tempfile.TemporaryDirectory() as directory:
                variant = Path(directory) / "16.pbrt"
                diagnostic = "[ tid 123 @ 0.1s cpu/integrators.cpp:266 ] ERROR Not-a-number radiance. Setting to black.\n"
                def execute(command, **kwargs):
                    variant.with_suffix(".pfm").write_bytes(
                        b"PF\n8 8\n-1\n" + struct.pack("<192f", *([0.] * 192)))
                    return subprocess.CompletedProcess(command, 0,
                        diagnostic if stream == "stdout" else "",
                        diagnostic if stream == "stderr" else "")
                runner = mock.Mock(side_effect=execute)
                with self.assertRaisesRegex(ValueError, "renderer reported ERROR"):
                    render_pfm("pbrt", Path("pbrt"), variant, b"WorldBegin\n", runner=runner)
                self.assertEqual(variant.with_suffix(f".{stream}.log").read_text(), diagnostic)
                record = json.loads(variant.with_suffix(".process.json").read_text())
                self.assertEqual(record["returncode"], 0)
                self.assertTrue(record["error_diagnostic"])
                self.assertEqual(runner.call_count, 1)

    def test_fatal_diagnostic_is_rejected_before_missing_pfm_readback(self):
        with tempfile.TemporaryDirectory() as directory:
            variant = Path(directory) / "16.pbrt"
            runner = mock.Mock(return_value=subprocess.CompletedProcess(
                [], 0, "\x1b[31mFATAL\x1b[0m invalid renderer state\n", ""))
            with self.assertRaisesRegex(ValueError, "renderer reported FATAL"):
                render_pfm("pbrt", Path("pbrt"), variant, b"WorldBegin\n", runner=runner)
            self.assertIn("FATAL", variant.with_suffix(".stdout.log").read_text())

    def test_clean_render_and_warning_pass_and_keep_transcripts(self):
        with tempfile.TemporaryDirectory() as directory:
            variant = Path(directory) / "00.pbrt"
            def execute(command, **kwargs):
                variant.with_suffix(".pfm").write_bytes(
                    b"PF\n8 8\n-1\n" + struct.pack("<192f", *([1.] * 192)))
                return subprocess.CompletedProcess(command, 0, "complete\n", "WARNING reference warning\n")
            pixels = render_pfm("rypath", Path("rypath"), variant, b"WorldBegin\n", runner=execute)
            self.assertEqual(pixels, [(1., 1., 1.)] * 64)
            self.assertEqual(variant.with_suffix(".stdout.log").read_text(), "complete\n")
            self.assertFalse(json.loads(variant.with_suffix(".process.json").read_text())["error_diagnostic"])

    def test_failure_and_timeout_retain_partial_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            variant = Path(directory) / "00.pbrt"
            runner = mock.Mock(return_value=subprocess.CompletedProcess([], 7, "partial\n", "failed\n"))
            with self.assertRaises(subprocess.CalledProcessError):
                render_pfm("rypath", Path("rypath"), variant, b"WorldBegin\n", runner=runner)
            self.assertEqual(variant.with_suffix(".stderr.log").read_text(), "failed\n")
            timeout = mock.Mock(side_effect=subprocess.TimeoutExpired([], 120,
                               output=b"partial timeout\n", stderr=b"FATAL timed out\n"))
            with self.assertRaises(subprocess.TimeoutExpired):
                render_pfm("rypath", Path("rypath"), variant, b"WorldBegin\n", runner=timeout)
            self.assertEqual(variant.with_suffix(".stdout.log").read_text(), "partial timeout\n")
            self.assertTrue(json.loads(variant.with_suffix(".process.json").read_text())["timed_out"])


class ComparisonTests(unittest.TestCase):
    def test_relocated_seed_entries_keep_verified_dependencies(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            fixture = root / "ply-test.pbrt"
            fixture.write_bytes(b'Sampler "independent" "integer seed" [0] '
                                b'"integer pixelsamples" [4096]\n'
                                b'Shape "plymesh" "string filename" ["nested/mesh.ply"]\n')
            asset = root / "nested" / "mesh.ply"
            asset.parent.mkdir()
            payload = b"verified dependency bytes\x00\xff"
            asset.write_bytes(payload)
            manifest = {fixture.stem: [{"path": "nested/mesh.ply", "bytes": len(payload),
                                       "sha256": hashlib.sha256(payload).hexdigest()}]}
            (root / "dependencies.json").write_text(json.dumps(manifest))
            visited = []

            def render(command, *, check, cwd, timeout, capture_output, text):
                entry, output = map(Path, command[-2:])
                if command[0] == "pbrt":
                    output, entry = entry, output
                self.assertEqual((entry.parent / "nested/mesh.ply").read_bytes(), payload)
                self.assertEqual(cwd, entry.parent)
                self.assertIn(b'nested/mesh.ply', entry.read_bytes())
                output.write_bytes(b"PF\n8 8\n-1\n" + struct.pack("<192f", *([1.] * 192)))
                visited.append(entry)
                return subprocess.CompletedProcess(command, 0, "", "")

            with mock.patch("compare_pbrt.subprocess.run", side_effect=render):
                result = render_replicates(Path("rypath"), Path("pbrt"), fixture,
                                           root / "runs", {"full": (0, 0, 8, 8)})
            self.assertEqual(len(visited), 32)
            self.assertEqual(result["rypath"]["full"], [(1., 1., 1.)] * 16)
            asset.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                stage_dependencies(fixture, root / "invalid")

    def test_triangle_filter_compares_full_frame_and_quadrants(self):
        self.assertEqual(REGIONS["triangle-filter"], {
            "full": (0, 0, 8, 8),
            "top-left": (0, 0, 4, 4),
            "top-right": (4, 0, 8, 4),
            "bottom-left": (0, 4, 4, 8),
            "bottom-right": (4, 4, 8, 8),
        })

    def test_filter_sensitivity_uses_each_backends_first_seed_and_signed_red_roi(self):
        for filter_name, region, sign in (("trilinear", "blend", 1), ("point", "nearest", -1)):
            with self.subTest(filter_name=filter_name):
                self.check_filter_control(filter_name, region, sign)

    def check_filter_control(self, filter_name, region, sign):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            scene = f"image-{filter_name}"
            entries = {"rypath": "00", "pbrt": "16"}
            measurements = {backend: {region: [(0.5, 0.5, 0.5)]}
                            for backend in entries}
            for backend, seed in entries.items():
                entry = root / scene / backend / f"{seed}.pbrt"
                entry.parent.mkdir(parents=True)
                entry.write_bytes(f'"integer seed" [{int(seed)}]\n'.encode() +
                                  f'"string filter" "{filter_name}"\n'.encode())

            def render(backend, binary, variant, source):
                seed = entries[backend]
                self.assertEqual(binary, Path(backend))
                self.assertEqual(variant, root / scene / backend /
                                 f"{seed}-bilinear.pbrt")
                self.assertEqual(source, f'"integer seed" [{int(seed)}]\n'.encode() +
                                 b'"string filter" "bilinear"\n')
                margin = failing_margin if backend == failing_backend else 0.03
                red = 0.5 + sign * margin
                return [(red if x == 4 and y in (3, 4) else 0.5 + sign * 0.1,
                         0.5 + sign * 0.1, 0.5 + sign * 0.1)
                        for y in range(8) for x in range(8)]

            with (mock.patch("compare_pbrt.render_pfm", side_effect=render),
                  contextlib.redirect_stdout(io.StringIO())):
                for failing_backend, failing_margin in ((None, 0), ("rypath", 0.01), ("pbrt", -0.05)):
                    with self.subTest(failing_backend=failing_backend, failing_margin=failing_margin):
                        self.assertEqual(check_filter_sensitivity(
                            scene, Path("rypath"), Path("pbrt"), root, measurements,
                        ), failing_backend is None)

    def test_point_control_rejects_equality_at_threshold(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for backend, seed in (("rypath", "00"), ("pbrt", "16")):
                entry = root / "image-point" / backend / f"{seed}.pbrt"
                entry.parent.mkdir(parents=True)
                entry.write_bytes(b'"string filter" "point"\n')
            measurements = {"rypath": {"nearest": [(0.03, 0, 0)]},
                            "pbrt": {"nearest": [(0.02, 0, 0)]}}
            with (mock.patch("compare_pbrt.render_pfm", return_value=[(0, 0, 0)] * 64),
                  contextlib.redirect_stdout(io.StringIO())):
                self.assertFalse(check_filter_sensitivity(
                    "image-point", Path("rypath"), Path("pbrt"), root, measurements,
                ))

    def test_filter_control_rejects_missing_or_duplicate_filter_token(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for filter_name in ("trilinear", "point"):
                scene = f"image-{filter_name}"
                entry = root / scene / "rypath" / "00.pbrt"
                entry.parent.mkdir(parents=True)
                for count in (0, 2):
                    with self.subTest(filter_name=filter_name, count=count):
                        entry.write_bytes(f'"string filter" "{filter_name}"\n'.encode() * count)
                        with (mock.patch("compare_pbrt.render_pfm") as render,
                              self.assertRaisesRegex(ValueError, 'expected one "string filter"')):
                            check_filter_sensitivity(scene, Path("rypath"), Path("pbrt"), root, {})
                        render.assert_not_called()

    def test_ewa_controls_use_independent_variants_and_signed_first_seed_roi(self):
        scene = "image-ewa"
        self.assertEqual(REGIONS[scene], {"full": (0, 0, 8, 8), "ellipse": (4, 3, 5, 4)})
        entries = {"rypath": "00", "pbrt": "16"}
        source = (b'"string filter"\n  [ "ewa" ] # "string filter" "ewa"\n'
                  b'"float maxanisotropy" [ 8 ] # "float maxanisotropy" [8]\n'
                  rb'"string label" "escaped \"float maxanisotropy\" [8]"' b"\n")
        variants = {
            "trilinear": source.replace(b'[ "ewa" ]', b'[ "trilinear" ]'),
            "ewa4": source.replace(b'[ 8 ]', b'[ 4 ]'),
        }
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            measurements = {backend: {"ellipse": [(0, 0, 0), (1, 0, 0)]}
                            for backend in entries}
            for backend, seed in entries.items():
                entry = root / scene / backend / f"{seed}.pbrt"
                entry.parent.mkdir(parents=True)
                seed_token = f'"integer seed" [{int(seed)}]\n'.encode()
                entry.write_bytes(seed_token + source)

            def render(backend, binary, variant, entry_source):
                seed = entries[backend]
                control = variant.stem.split("-", 1)[1]
                self.assertEqual(binary, Path(backend))
                self.assertEqual(variant, root / scene / backend / f"{seed}-{control}.pbrt")
                self.assertEqual(entry_source, f'"integer seed" [{int(seed)}]\n'.encode() +
                                 variants[control])
                visited.add((backend, control))
                red = -failing_margin if (backend, control) == failing_control else -0.03
                return [(red if (x, y) == (4, 3) else -1, -1, -1)
                        for y in range(8) for x in range(8)]

            with (mock.patch("compare_pbrt.render_pfm", side_effect=render),
                  contextlib.redirect_stdout(io.StringIO())):
                for failing_control, failing_margin in (
                    (None, 0), (("rypath", "trilinear"), 0.01),
                    (("pbrt", "trilinear"), -0.05), (("rypath", "ewa4"), -0.05),
                    (("pbrt", "ewa4"), 0.02),
                ):
                    with self.subTest(failing_control=failing_control, failing_margin=failing_margin):
                        visited = set()
                        self.assertEqual(check_filter_sensitivity(
                            scene, Path("rypath"), Path("pbrt"), root, measurements,
                        ), failing_control is None)
                        self.assertEqual(visited, {(backend, control) for backend in entries
                                                   for control in variants})

    def test_ewa_control_rejects_missing_duplicate_or_wrong_parameters(self):
        scene = "image-ewa"
        trilinear, ewa4 = FILTER_CONTROLS[scene]
        for control, source in (
            (trilinear, b'# "string filter" "ewa"'),
            (trilinear, b'"string filter" "ewa" "string  filter" "bilinear"'),
            (trilinear, b'"string filter" ["ewa" "point"]'),
            (trilinear, b'"string filter" "ewax"'),
            (ewa4, b'# "float maxanisotropy" [8]'),
            (ewa4, b'"float maxanisotropy" [8] "float maxanisotropy" [4]'),
            (ewa4, b'"float maxanisotropy" [80]'),
            (ewa4, b'"float maxanisotropy" [8'),
        ):
            with self.subTest(control=control.alternate_name, source=source):
                with self.assertRaisesRegex(ValueError, "expected one"):
                    make_filter_variant(scene, source, control)

    def test_rois_use_top_down_coordinates(self):
        pixels = [(x, y, 8 * y + x) for y in range(8) for x in range(8)]
        self.assertEqual(roi_means(pixels, (4, 0, 8, 4)), (5.5, 1.5, 17.5))
        self.assertEqual(roi_means(pixels, (0, 4, 4, 8)), (1.5, 5.5, 45.5))

    def test_channel_gate_includes_reference_relative_floor(self):
        reference = {"full": [(0, 0, 10)] * 16}
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(
                compare_regions("test", {"pbrt": reference, "rypath": {"full": [(0, 0, 10.0001)] * 16}})
            )
            self.assertFalse(
                compare_regions("test", {"pbrt": reference, "rypath": {"full": [(0, 0, 10.001)] * 16}})
            )


class CornellFixtureTests(unittest.TestCase):
    def test_derivative_uses_only_documented_edits(self):
        fixtures = Path(__file__).resolve().parents[1] / "tests" / "fixtures"
        original = (fixtures / "pbrt" / "cornell-box.pbrt").read_bytes()
        notice = (fixtures / "pbrt" / "LICENSE.txt").read_bytes()
        self.assertEqual(hashlib.sha256(original).hexdigest(),
                         "b2fc6654c966468119187d169a76ecec82e169a1058f3344e27e5103189d4e43")
        self.assertEqual(hashlib.sha256(notice).hexdigest(),
                         "520b18c04d27fd9abca81364746178a9a3d52cff3c6204de05be5b68e547ee43")

        expected = original
        for old, new in (
            (b'Sampler "sobol"\n    "integer pixelsamples" [ 64 ]',
             b'Sampler "independent"\n    "integer pixelsamples" [4096]\n    "integer seed" [0]'),
            (b'PixelFilter "triangle"\n    "float xradius" [ 1 ]\n    "float yradius" [ 1 ]',
             b'PixelFilter "box"\n    "float xradius" [0.5]\n    "float yradius" [0.5]'),
            CORNELL_FILM_EDIT,
        ):
            self.assertEqual(expected.count(old), 1)
            expected = expected.replace(old, new, 1)
        for parameter in (b"normal N", b"point2 uv"):
            pattern = rb'(?m)^[ \t]+"' + parameter + rb'"\s*\[[^\]]*\]\n'
            expected, removed = re.subn(pattern, b"", expected)
            self.assertEqual(removed, 8)
        adapted = (fixtures / "pbrt-render" / "cornell-box-adapted.pbrt").read_bytes()
        self.assertEqual(adapted, CORNELL_CREDIT + expected)

    def test_sobol_derivatives_preserve_the_original_scene(self):
        fixtures = Path(__file__).resolve().parents[1] / "tests" / "fixtures"
        original = (fixtures / "pbrt" / "cornell-box.pbrt").read_bytes()
        film_token = CORNELL_FILM_EDIT[0]
        samples_token = b'"integer pixelsamples" [ 64 ]'
        self.assertEqual(original.count(film_token), 1)
        self.assertEqual(original.count(samples_token), 1)
        base = original.replace(*CORNELL_FILM_EDIT, 1)
        for mode in ("fastowen", "permutedigits", "owen"):
            with self.subTest(mode=mode):
                scene = f"cornell-box-sobol-{mode}"
                adapted = (fixtures / "pbrt-render" / f"{scene}.pbrt").read_bytes()
                expected = base.replace(
                    samples_token,
                    b'"integer pixelsamples" [4096]\n    "integer seed" [0]\n'
                    + f'    "string randomization" "{mode}"'.encode(),
                    1,
                )
                self.assertEqual(adapted, CORNELL_CREDIT + expected)
                self.assertEqual(REGIONS[scene], QUADRANTS)

    def test_all_area_fixtures_compare_full_frame_and_quadrants(self):
        for scene in ("area-visible", "area-direct", "area-mixed", "cornell-box-adapted"):
            with self.subTest(scene=scene):
                self.assertEqual(REGIONS[scene], {
                    "full": (0, 0, 8, 8),
                    "top-left": (0, 0, 4, 4),
                    "top-right": (4, 0, 8, 4),
                    "bottom-left": (0, 4, 4, 8),
                    "bottom-right": (4, 4, 8, 8),
                })


if __name__ == "__main__":
    unittest.main()
