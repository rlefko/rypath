# SPDX-License-Identifier: GPL-3.0-only
"""GPU-independent qualification input and failed-publication checks."""
import contextlib
import io
import json
import os
from pathlib import Path
import runpy
import signal
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch


def run(executable, arguments):
    return subprocess.run([str(executable), *map(str, arguments)], capture_output=True,
                          text=True, timeout=20, check=False)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        assert key not in result, f"Duplicate report key: {key}"
        result[key] = value
    return result


def runner_checks():
    """Real runner orchestration with a CPU fake child, never a Metal process."""
    runner_path = Path(__file__).resolve().parents[1] / "scripts/run_pipeline_benchmarks.py"
    namespace = runpy.run_path(str(runner_path), run_name="pipeline_benchmarks_test")
    runner = namespace["main"]
    with tempfile.TemporaryDirectory(prefix="rypath-pipeline-runner-cli-") as temporary:
        root = Path(temporary)
        scene = root / "source.pbrt"
        scene.write_text("# CPU fake input; not rendered\n")
        executable = root / "fake benchmark"
        executable.write_text("#!" + sys.executable + "\n" + r'''
import json, os, sys, time
from pathlib import Path
args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
if os.environ.get('RYPATH_FAKE_SLEEP') == '1':
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline and not Path(args['--output'] + '.stop').exists():
        time.sleep(.01)
    sys.exit(99)
assert os.environ['MTL_DEBUG_LAYER'] == os.environ['MTL_SHADER_VALIDATION'] == '0'
w, h, n, warmup, seed = (int(args[k]) for k in ('--width', '--height', '--frames', '--warmup', '--seed'))
mode = args['--reconstruction']
if os.environ.get('RYPATH_FAKE_SUBSTITUTE') == '1': mode = 'metalfx'
settings = {'width': w, 'height': h, 'output_width': w, 'output_height': h, 'seed': seed,
 'material_seed': 0, 'execution': 'wavefront', 'intersection_profile': 'native', 'target_scale': 1,
 'estimator': args['--estimator'], 'effective_mutation_steps': int(args['--mutations']),
 'reconstruction': mode, 'footprint_samples': 1,
 'area_contract': 7 if args['--estimator'] == 'area-replay' else 0,
 'requested_area_temporal_cache': True, 'area_temporal_cache': args['--estimator'] == 'area-replay',
 'area_output': args.get('--area-output', 'conditional-mean') if args['--estimator'] == 'area-replay' else 'not-applicable',
 'area_target': 'quarter-l1-v1' if args['--estimator'] == 'area-replay' else 'none',
 'area_source_mis': 'source-target-mis-v1' if args['--estimator'] == 'area-replay' else 'none',
 'area_temporal_reuse': 20 if args['--estimator'] == 'area-replay' else 0}
if args['--estimator'] == 'independent': assert '--area-output' not in args
if os.environ.get('RYPATH_FAKE_LEGACY_AREA') == '1': settings['area_contract'] = 3
metric = {'cpu_wall': {'count': n, 'median_seconds': .01, 'p95_seconds': .02}}
report = {'status': 'complete', 'exit_code': 0, 'diagnostic': '', 'settings': settings,
 'requested_frames': n, 'completed_frames': n, 'requested_warmup': warmup, 'completed_warmup': warmup,
 'consumed_sample_indices': n + warmup, 'source_identity': 'cpu-fake-effective-source',
 'build': 'CPU fake only', 'device': {'name': 'CPU fake only'}, 'startup_seconds': .001,
 'frames': [{'complete': True, 'pipeline_complete': True, 'sample_index': i, 'warmup': i < warmup,
             'completed_samples': w*h,
             'history_used': i > 0 and args['--estimator'] == 'area-replay',
             'completed_evaluations': w*h*(1 if args['--estimator'] == 'independent' else
                 ((7 if i > 0 else 5)+int(args['--mutations'])))} for i in range(n + warmup)],
 'summary': metric, 'pipeline_summary': metric}
for row in report['frames']:
    row['temporal_evaluation_cache_used'] = row['history_used']
    row['logical_evaluations'] = row['completed_evaluations']
    row['cached_evaluations'] = 2*w*h if row['history_used'] else 0
    row['completed_evaluations'] -= row['cached_evaluations']
if os.environ.get('RYPATH_FAKE_BAD_CACHE') == '1':
    report['frames'][0]['temporal_evaluation_cache_used'] = True
if os.environ.get('RYPATH_FAKE_OLD_COUNTS') == '1':
    for row in report['frames']:
        if row['history_used']: row['completed_evaluations'] -= w*h
Path(args['--output']).write_text(json.dumps(report))
''')
        executable.chmod(0o755)
        (root / "rypath.metallib").write_bytes(b"CPU fake; never loaded")
        base = ["--executable", str(executable), "--scene", "owned=" + str(scene),
                "--width", "2", "--height", "1", "--frames", "2", "--warmup", "1",
                "--seeds", "31001,31002", "--controls"]

        def invoke(arguments):
            handlers = {number: signal.getsignal(number)
                        for number in (signal.SIGINT, signal.SIGTERM)}
            try:
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    try:
                        return runner(arguments)
                    except SystemExit as error:
                        return error.code
            finally:
                assert all(signal.getsignal(number) == handler
                           for number, handler in handlers.items()), "Runner changed caller handlers"

        # Skip platform/toolchain queries in this fake test, explicitly labeling
        # the resulting provenance as test data. Child orchestration stays real.
        queries = {"provenance": lambda *_: {"cpu_fake_test": True},
                   "command_info": lambda command: {"command": command, "exit_code": None,
                                                     "unavailable_or_timeout": "CPU fake test"}}
        with patch.dict(runner.__globals__, queries):
            for requested, area_output in ((None, "conditional-mean"), ("temporal", "conditional-mean"), ("metalfx", "conditional-mean"), ("temporal", "selected")):
                expected = requested or "temporal"
                destination = root / ((requested or "default") + "-" + area_output)
                arguments = base + ["--output-dir", str(destination), "--area-output", area_output]
                if requested:
                    arguments += ["--reconstruction", requested]
                assert invoke(arguments) == 0
                manifest = json.loads((destination / "manifest.json").read_text(),
                                      object_pairs_hook=unique_object)
                summary = json.loads((destination / "summary.json").read_text(),
                                     object_pairs_hook=unique_object)
                assert manifest["complete"] and summary["complete"]
                assert manifest["primary_reconstruction"] == summary["primary_reconstruction"] == expected
                assert manifest["area_output"] == summary["area_output"] == area_output
                assert manifest["area_contract"] == summary["area_contract"] == 7
                assert summary["area_source_mis"] == "source-target-mis-v1"
                assert len(manifest["runs"]) == 13
                modes = [row["reconstruction"] for row in manifest["runs"]]
                assert modes.count(expected) == 10 and modes.count("none") == 2 and modes.count("raw-rgb") == 1
                for row in manifest["runs"]:
                    assert row["complete"] and row["benchmark"]["settings"]["reconstruction"] == row["reconstruction"]
                    child = json.loads(Path(row["report"]).read_text())
                    assert child["settings"]["reconstruction"] == row["reconstruction"]
                    assert child["settings"]["area_output"] == (area_output if row["estimator"] == "area-replay" else "not-applicable")
                    assert ("--area-output" in row["command"]) is (row["estimator"] == "area-replay")
                retained = (destination / "manifest.json").read_bytes()
                assert invoke(arguments) == 2
                assert (destination / "manifest.json").read_bytes() == retained
            for invalid in ("none", "raw-rgb", "unknown"):
                destination = root / ("invalid-" + invalid)
                assert invoke(base + ["--output-dir", str(destination), "--reconstruction", invalid]) == 2
                assert not destination.exists()
            destination = root / "invalid-area-output"
            assert invoke(base + ["--output-dir", str(destination), "--area-output", "conditional_mean"]) == 2
            assert not destination.exists()
            destination = root / "substitution"
            with patch.dict(os.environ, {"RYPATH_FAKE_SUBSTITUTE": "1"}):
                assert invoke(base + ["--output-dir", str(destination)]) == 1
            manifest = json.loads((destination / "manifest.json").read_text())
            assert not manifest["complete"] and len(manifest["runs"]) == 1
            assert "reconstruction" in manifest["failure"]
            for kind, expected_failure in (("LEGACY_AREA", "area_contract"), ("OLD_COUNTS", "evaluation count"),
                                           ("BAD_CACHE", "cache permission")):
                destination = root / ("rejected-" + kind)
                with patch.dict(os.environ, {"RYPATH_FAKE_" + kind: "1"}):
                    assert invoke(base + ["--output-dir", str(destination)]) == 1
                rejected = json.loads((destination / "manifest.json").read_text())
                assert not rejected["complete"] and expected_failure in rejected["failure"]
            # Deliver a signal from inside Popen, after creation but BEFORE main
            # receives the child handle. A non-raising handler must let it take
            # ownership and reap that exact process.
            actual_popen = subprocess.Popen
            launched = []

            def launch_then_cancel(*args, **kwargs):
                child = actual_popen(*args, **kwargs)
                launched.append(child)
                os.kill(os.getpid(), signal.SIGTERM)
                return child

            destination = root / "launch-window-cancel"
            with patch.dict(os.environ, {"RYPATH_FAKE_SLEEP": "1"}), \
                    patch("subprocess.Popen", side_effect=launch_then_cancel):
                assert invoke(base + ["--output-dir", str(destination)]) == 143
            assert len(launched) == 1 and launched[0].returncode is not None
            cancelled = json.loads((destination / "manifest.json").read_text())
            assert not cancelled["complete"] and cancelled["exit_code"] == 143
            assert cancelled["runs"][0]["child_pid"] == launched[0].pid
            assert cancelled["runs"][0]["active_pid"] is None

        # Exercise real OS delivery to a separate runner process and its sleeping
        # CPU-only child. Only expensive platform probes are replaced here.
        wrapper = root / "runner-wrapper.py"
        wrapper.write_text(r'''
import runpy, sys
namespace = runpy.run_path(sys.argv[1], run_name="pipeline_runner_child_test")
main = namespace["main"]
main.__globals__["provenance"] = lambda *_: {"cpu_fake_test": True}
main.__globals__["command_info"] = lambda command: {"command": command, "exit_code": None,
    "unavailable_or_timeout": "CPU fake signal test"}
sys.exit(main(sys.argv[2:]))
''')
        for number in (signal.SIGINT, signal.SIGTERM):
            destination = root / ("signal-" + signal.Signals(number).name)
            process = subprocess.Popen(
                [sys.executable, str(wrapper), str(runner_path), *base,
                 "--output-dir", str(destination)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                env={**os.environ, "RYPATH_FAKE_SLEEP": "1"})
            child_pid, child_report = None, None
            try:
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and process.poll() is None:
                    manifest_path = destination / "manifest.json"
                    if manifest_path.exists():
                        current = json.loads(manifest_path.read_text())
                        if current["runs"] and current["runs"][-1].get("active_pid"):
                            child_pid = current["runs"][-1]["active_pid"]
                            child_report = current["runs"][-1]["report"]
                            break
                    time.sleep(.01)
                assert child_pid is not None, "Runner did not publish its active child PID"
                os.kill(process.pid, number)
                stdout, stderr = process.communicate(timeout=10)
                assert process.returncode == 128 + number, (stdout, stderr)
                for name in ("manifest.json", "summary.json"):
                    stopped = json.loads((destination / name).read_text())
                    assert not stopped["complete"] and stopped["stopped"]
                    assert stopped["exit_code"] == 128 + number
                    assert signal.Signals(number).name in stopped["failure"]
                    row = stopped["runs"][0]
                    assert row["child_pid"] == child_pid and row["active_pid"] is None
                    assert row["exit_code"] is not None
                try:
                    os.kill(child_pid, 0)
                except ProcessLookupError:
                    pass
                else:
                    raise AssertionError("Cancelled runner left its benchmark child alive")
            finally:
                # Cooperative fake-child cleanup on assertion failures avoids
                # ever signaling a PID after the runner may have reaped it.
                if child_report is not None:
                    Path(child_report + ".stop").touch()
                if process.poll() is None:
                    process.terminate()
                try:
                    process.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=10)
                if child_pid is not None and process.returncode != 128 + number:
                    deadline = time.monotonic() + 2
                    while time.monotonic() < deadline:
                        try:
                            os.kill(child_pid, 0)
                        except ProcessLookupError:
                            break
                        time.sleep(.01)
    print("CPU fake runner mode, controls, metadata, fresh-output, no-substitution and cancellation checks passed")


def qualification_checks(executable):
    help_result = run(executable, ["--help"])
    assert help_result.returncode == 0 and "reference-spp" in help_result.stdout
    assert all(word in help_result.stdout for word in ("reconstruction", "temporal", "metalfx", "--area-output"))
    with tempfile.TemporaryDirectory(prefix="rypath-qualification-cli-") as temporary:
        root = Path(temporary)
        destination = root / "unused"
        source = root / "missing.pbrt"
        base = ["--scene", source, "--output-dir", destination]
        invalid = [
            base + ["--hero-wavelengths", "invalid"],
            base + ["--area-mutation-policy", "invalid"],
            base + ["--area-stencil", "invalid"],
            [], ["--scene"], base + ["--unknown", "1"],
            base + ["--width", "0"], base + ["--height", "16385"],
            base + ["--width", "4294967296"],
            base + ["--width", "16384", "--height", "16384"],
            base + ["--frames", "0"], base + ["--frames", "1000001"],
            base + ["--seeds", "4097"], base + ["--seeds", "-1"],
            base + ["--reference-batches", "1"],
            base + ["--reference-spp", "3", "--reference-batches", "2"],
            base + ["--reference-spp", "2", "--reference-batches", "4"],
            base + ["--width", "4", "--width", "8"],
            base + ["--motion-offset", "0"], base + ["--motion-offset", "nan"],
            base + ["--motion-offset", "inf"], base + ["--motion-offset", "1junk"],
            base + ["--motion-offset", ".25", "--frames", "7"],
            base + ["--motion-offset", ".25", "--frames", "4"],
            base + ["--area-output"], base + ["--area-output", "invalid"],
            base + ["--area-output", "selected", "--area-output", "conditional-mean"],
            base + ["--reconstruction"], base + ["--reconstruction", "none"],
            base + ["--reconstruction", "raw-rgb"], base + ["--reconstruction", "unknown"],
            base + ["--reconstruction", "temporal", "--reconstruction", "metalfx"],
        ]
        for arguments in invalid:
            result = run(executable, arguments)
            assert result.returncode != 0 and result.stderr, arguments
            assert not destination.exists(), "Invalid options created output"
        existing = root / "existing"
        existing.mkdir()
        sentinel = existing / "summary.json"
        sentinel.write_text("retained\n")
        result = run(executable, ["--scene", source, "--output-dir", existing])
        assert result.returncode != 0 and "fresh" in result.stderr.lower()
        assert sentinel.read_text() == "retained\n"
        assert sorted(path.name for path in existing.iterdir()) == ["summary.json"]
        # A source failure after reservation must remain distinguishable from a
        # completed GPU run, including on the portable backend without a device.
        for selected in (None, "temporal", "metalfx"):
            destination = root / (selected or "default-mode")
            arguments = ["--scene", source, "--output-dir", destination]
            if selected:
                arguments += ["--reconstruction", selected]
            result = run(executable, arguments)
            assert result.returncode != 0 and result.stderr
            report = json.loads((destination / "summary.json").read_text(),
                                object_pairs_hook=unique_object)
            assert report["complete"] is False
            assert report["schema"] == 2
            assert report["settings"]["area_contract"] == 14
            assert report["settings"]["area_output"] == "conditional-mean"
            assert report["settings"]["area_target"] == "quarter-l1-v1"
            assert report["settings"]["area_source_mis"] == "source-target-mis-v1"
            assert report["settings"]["area_temporal_reuse"] == 20
            expected = selected or "temporal"
            assert report["settings"]["reconstruction"] == expected
            assert report["settings"]["reconstruction_resolution"] == "native"
            assert report["settings"]["reconstructed_output_kind"].startswith(expected + "-denoised-")
            assert report["settings"]["reconstruction_substitution"] is False
            assert "signed" in report["physical_metric"]
            assert "separately clamp" in report["presentation_metric"]
            assert not list(destination.glob("*.exr"))
        destination = root / "selected-area-output"
        result = run(executable, ["--scene", source, "--output-dir", destination, "--area-output", "selected"])
        assert result.returncode != 0
        selected = json.loads((destination / "summary.json").read_text())
        assert selected["schema"] == 2 and selected["settings"]["area_output"] == "selected"
        assert selected["settings"]["area_contract"] == 14
    print("Qualification CLI and failure publication checks passed")


if __name__ == "__main__":
    if sys.argv[1:] != ["--runner-only"]:
        qualification_checks(Path(sys.argv[1]).resolve())
    runner_checks()
