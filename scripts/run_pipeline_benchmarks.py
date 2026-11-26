#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the serial native pipeline matrix; caller must ensure GPU quiescence.

Child wall/RSS include startup and output. Frame statistics come unchanged from
the benchmark report. This runner neither measures image quality nor certifies
that other GPU users are idle. It never replaces failed runs or existing output.
"""

import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
ARMS = [("independent", "independent", 0)] + [
    (f"area{n}", "area-replay", n) for n in (0, 1, 2, 4)]


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def identity(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return {"path": str(path), "sha256": digest.hexdigest()}


def command_info(command):
    try:
        r = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=15)
        return {"command": command, "exit_code": r.returncode,
                "stdout": r.stdout, "stderr": r.stderr}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"command": command, "exit_code": None, "unavailable_or_timeout": str(error)}


def provenance(executable, scenes):
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    # Only the boolean is retained: never publish local/untracked path listings.
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "-z"], cwd=ROOT))
    commands = [["clang", "--version"]]
    if sys.platform == "darwin":
        commands += [["sw_vers"], ["sysctl", "hw.model", "machdep.cpu.brand_string", "hw.memsize"],
                     ["xcodebuild", "-version"], ["xcrun", "--sdk", "macosx", "--show-sdk-version"],
                     ["xcrun", "--sdk", "macosx", "metal", "--version"]]
    return {"binary": identity(executable), "metallib": identity(executable.parent / "rypath.metallib"),
            "runner": identity(Path(__file__).resolve()), "repository_revision": revision,
            "repository_dirty": dirty, "platform": platform.platform(), "uname": dict(platform.uname()._asdict()),
            "hardware_toolchain": [command_info(c) for c in commands],
            "scenes": {name: identity(path) for name, path in scenes}}


def publish(path, value):
    temporary = path.with_suffix(path.suffix + ".pending")
    created = False
    try:
        with temporary.open("x") as stream:
            created = True
            json.dump(value, stream, indent=2, allow_nan=False)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        if created:
            temporary.unlink(missing_ok=True)


class Cancelled(Exception):
    def __init__(self, number):
        self.number = number


class Cancellation:
    def __init__(self):
        self.number = None
        self.requests = 0

    def request(self, number, _frame):
        # Never raise from a handler: a signal may arrive after Popen creates
        # its child but before the caller receives the child object.
        if self.number is None:
            self.number = number
        self.requests += 1

    def check(self):
        if self.number is not None:
            raise Cancelled(self.number)


def reap(child, interrupted=False, cancellation=None):
    """Reap only our child, retaining wait4's per-process peak RSS."""
    cancellation = cancellation or Cancellation()
    deadline, sent_requests = None, 0
    signaled = False
    while True:
        try:
            # Publish reaped status before a pending signal can trigger cleanup.
            # A reaped PID may be reused; it must never be signaled afterward.
            mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
            try:
                pid, status, usage = os.wait4(child.pid, os.WNOHANG)
                if pid:
                    child.returncode = os.waitstatus_to_exitcode(status)
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, mask)
            if pid:
                return usage, interrupted or cancellation.number is not None
            if interrupted or cancellation.number is not None:
                if not signaled:
                    number = cancellation.number or signal.SIGINT
                    deadline = time.monotonic() + 5
                    signaled, sent_requests = True, cancellation.requests
                elif deadline is not None and (time.monotonic() >= deadline or
                                               cancellation.requests > sent_requests):
                    number, deadline = signal.SIGKILL, None
                else:
                    number = None
                if number is not None:
                    try:
                        os.kill(child.pid, number)
                    except ProcessLookupError:
                        pass  # An exited, unreaped child still owns its PID.
            time.sleep(.02)
        except KeyboardInterrupt:
            interrupted = True
            if child.returncode is not None:
                return usage, interrupted
            cancellation.request(signal.SIGINT, None)


def reject_constant(value):
    raise ValueError(f"Nonfinite JSON constant: {value}")


def inspect_report(report, expected, frames, warmup):
    if not isinstance(report.get("settings"), dict):
        raise ValueError("Missing benchmark settings object")
    if report["status"] != "complete" or report["exit_code"] != 0:
        raise ValueError(f"Benchmark {report['status']}: {report.get('diagnostic', '')}")
    for key, value in expected.items():
        if report["settings"].get(key) != value:
            raise ValueError(f"Report setting mismatch: {key}")
    for key, value in (("requested_frames", frames), ("completed_frames", frames),
                       ("requested_warmup", warmup), ("completed_warmup", warmup),
                       ("consumed_sample_indices", frames + warmup)):
        if report[key] != value:
            raise ValueError(f"Report count mismatch: {key}")
    if len(report["frames"]) != frames + warmup:
        raise ValueError("Incomplete frame observations")
    for index, row in enumerate(report["frames"]):
        if (row["complete"] is not True or row["pipeline_complete"] is not True or
                row["sample_index"] != index or row["warmup"] is not (index < warmup) or
                row["completed_samples"] != expected["width"] * expected["height"]):
            raise ValueError(f"Incomplete/mismatched frame observation: {index}")
        if not isinstance(row.get("history_used"), bool):
            raise ValueError(f"Missing actual history accounting: {index}")
        evaluations = 1 if expected["estimator"] == "independent" else (
            (7 if row["history_used"] else 5) + expected["effective_mutation_steps"])
        pixels = expected["width"] * expected["height"]
        logical = pixels * evaluations
        cache_used = row.get("temporal_evaluation_cache_used")
        if not isinstance(cache_used, bool) or (cache_used and (
                expected["estimator"] != "area-replay" or not row["history_used"] or
                report["settings"].get("area_temporal_cache") is not True)):
            raise ValueError(f"Invalid exact temporal cache permission: {index}")
        cached = 2 * pixels if cache_used else 0
        if (row.get("completed_evaluations") != logical - cached or
                row.get("cached_evaluations") != cached or row.get("logical_evaluations") != logical):
            raise ValueError(f"Source-MIS evaluation count mismatch: {index}")
    for name in ("summary", "pipeline_summary"):
        metric = report[name]["cpu_wall"]
        if metric["count"] != frames or any(not isinstance(metric[k], (float, int)) or
                not math.isfinite(metric[k]) or metric[k] < 0 for k in ("median_seconds", "p95_seconds")):
            raise ValueError(f"Incomplete/nonfinite {name} wall statistics")
    if not isinstance(report["source_identity"], str) or not report["source_identity"]:
        raise ValueError("Missing effective scene/asset source identity")
    return {key: report[key] for key in ("source_identity", "build", "device", "settings",
                                        "startup_seconds", "summary", "pipeline_summary")}


def matrix(scenes, seeds, controls, reconstruction="temporal"):
    indices = {name: i for i, (name, _) in enumerate(scenes)}
    for run_index, seed in enumerate(seeds):
        ordered = scenes[run_index % len(scenes):] + scenes[:run_index % len(scenes)]
        for name, path in ordered:
            shift = (run_index + indices[name]) % len(ARMS)
            arms = [(arm, reconstruction) for arm in ARMS[shift:] + ARMS[:shift]]
            if controls and run_index == 0:
                arms += [(ARMS[0], "none"), (ARMS[2], "none"), (ARMS[2], "raw-rgb")]
            for (arm, estimator, mutations), mode in arms:
                yield {"scene": name, "scene_path": str(path), "seed": seed, "arm": arm,
                       "estimator": estimator, "mutations": mutations, "reconstruction": mode}


def arguments(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--executable", required=True, type=Path)
    p.add_argument("--scene", action="append", required=True, metavar="NAME=PATH")
    p.add_argument("--output-dir", required=True, type=Path)
    for name, default in (("width", 1920), ("height", 1080), ("frames", 24), ("warmup", 4)):
        p.add_argument("--" + name, type=int, default=default)
    p.add_argument("--seeds", default="31001,31002,31003")
    p.add_argument("--reconstruction", choices=("temporal", "metalfx"), default="temporal",
                   help="explicit native primary-arm mode (default: temporal); no substitution")
    p.add_argument("--area-output", choices=("selected", "conditional-mean"), default="conditional-mean",
                   help="explicit Area output ablation; independent arms remain unchanged")
    p.add_argument("--controls", action="store_true", help="add three first-seed controls per scene")
    a = p.parse_args(argv)
    try:
        if sys.platform not in ("darwin", "linux") or not hasattr(os, "wait4"):
            raise ValueError("Runner requires macOS or Linux wait4/RSS semantics")
        if not (0 < a.width <= 16384 and 0 < a.height <= 16384 and
                0 < a.frames < 2**64 and 0 <= a.warmup < 2**64 - a.frames):
            raise ValueError("Invalid dimensions or frame/warmup bounds")
        seed_strings = a.seeds.split(",")
        if not all(re.fullmatch(r"[0-9]+", s) for s in seed_strings):
            raise ValueError("Seeds must be comma-separated unsigned integers")
        a.seeds = [int(s) for s in seed_strings]
        if not a.seeds or len(set(a.seeds)) != len(a.seeds) or any(s >= 2**64 for s in a.seeds):
            raise ValueError("Seeds must be unique uint64 values")
        scenes = []
        for spec in a.scene:
            name, separator, path = spec.partition("=")
            if not separator or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", name):
                raise ValueError("Scenes require a slug-safe NAME=PATH")
            path = Path(path).expanduser().resolve(strict=True)
            if not path.is_file():
                raise ValueError("Scene must be a regular file")
            scenes.append((name, path))
        if len({n.casefold() for n, _ in scenes}) != len(scenes):
            raise ValueError("Scene names must be unique, ignoring case")
        a.executable = a.executable.expanduser().resolve(strict=True)
        if not a.executable.is_file() or not os.access(a.executable, os.X_OK):
            raise ValueError("Executable must be an executable regular file")
        if not (a.executable.parent / "rypath.metallib").is_file():
            raise ValueError("Adjacent rypath.metallib is required")
        a.output_dir = a.output_dir.expanduser().absolute()
        if os.path.lexists(a.output_dir) or not a.output_dir.parent.is_dir():
            raise ValueError("Output directory must be fresh, with an existing parent")
        a.output_dir = a.output_dir.parent.resolve() / a.output_dir.name
        a.scenes = scenes
    except (OSError, ValueError) as error:
        p.error(str(error))
    return a


def main(argv=None):
    a = arguments(argv)
    plan = list(matrix(a.scenes, a.seeds, a.controls, a.reconstruction))
    a.output_dir.mkdir()
    manifest = {"schema": 1, "complete": False, "started": utc(), "requested_runs": len(plan),
                "requested_frames_per_run": a.frames, "requested_warmup_per_run": a.warmup,
                "primary_reconstruction": a.reconstruction,
                "area_contract": 7, "area_output": a.area_output,
                "area_target": "quarter-l1-v1", "area_source_mis": "source-target-mis-v1",
                "area_temporal_reuse": 20,
                "quiescence": "Caller must ensure no competing GPU work; not automatically verified",
                "timing_scope": "child wall/RSS include startup/output; frame summaries come from child JSON",
                "environment_overrides": {"MTL_DEBUG_LAYER": "0", "MTL_SHADER_VALIDATION": "0"},
                "peak_rss_source": "os.wait4 ru_maxrss: bytes on macOS, KiB*1024 on Linux",
                "peak_rss_scope": "Process resident high-water mark, not total Metal or MetalFX private allocations",
                "thermal_scope": "pmset OS thermal warnings, not full GPU temperature/clock telemetry",
                "plan": plan, "runs": []}
    code = 1
    cancellation = Cancellation()
    previous_handlers = {number: signal.getsignal(number)
                         for number in (signal.SIGINT, signal.SIGTERM)}
    for number in previous_handlers:
        signal.signal(number, cancellation.request)
    try:
        manifest["provenance"] = provenance(a.executable, a.scenes)
        manifest["thermal_before_matrix"] = command_info(["/usr/bin/pmset", "-g", "therm"])
        publish(a.output_dir / "manifest.json", manifest)
        for index, run in enumerate(plan):
            cancellation.check()
            stem = f"{index:03}-{run['scene']}-{run['arm']}-{run['reconstruction']}-{run['seed']}"
            report = a.output_dir / (stem + ".json")
            expected = {"width": a.width, "height": a.height, "output_width": a.width,
                        "output_height": a.height, "seed": run["seed"], "material_seed": 0,
                        "execution": "wavefront", "intersection_profile": "native", "target_scale": 1,
                        "estimator": run["estimator"], "effective_mutation_steps": run["mutations"],
                        "reconstruction": run["reconstruction"], "footprint_samples": 1,
                        "area_contract": 7 if run["estimator"] == "area-replay" else 0,
                        "requested_area_temporal_cache": True,
                        "area_temporal_cache": run["estimator"] == "area-replay",
                        "area_output": a.area_output if run["estimator"] == "area-replay" else "not-applicable",
                        "area_target": "quarter-l1-v1" if run["estimator"] == "area-replay" else "none",
                        "area_source_mis": "source-target-mis-v1" if run["estimator"] == "area-replay" else "none",
                        "area_temporal_reuse": 20 if run["estimator"] == "area-replay" else 0}
            command = [str(a.executable), "--scene", run["scene_path"], "--output", str(report),
                       "--width", str(a.width), "--height", str(a.height), "--frames", str(a.frames),
                       "--warmup", str(a.warmup), "--seed", str(run["seed"]), "--material-seed", "0",
                       "--execution", "wavefront", "--intersections", "native", "--target-scale", "1",
                       "--estimator", run["estimator"], "--mutations", str(run["mutations"]),
                       "--reconstruction", run["reconstruction"]]
            if run["estimator"] == "area-replay":
                command += ["--area-output", a.area_output]
            row = {**run, "command": command, "report": str(report), "started": utc(), "complete": False, "exit_code": None,
                   "child_pid": None, "active_pid": None,
                   "stdout_log": str(a.output_dir / (stem + ".stdout.log")),
                   "stderr_log": str(a.output_dir / (stem + ".stderr.log"))}
            manifest["runs"].append(row)
            publish(a.output_dir / "manifest.json", manifest)
            child, begin = None, time.monotonic()
            try:
                with open(row["stdout_log"], "x") as out, open(row["stderr_log"], "x") as err:
                    child = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True,
                                             env={**os.environ, **manifest["environment_overrides"]})
                    row.update(child_pid=child.pid, active_pid=child.pid)
                    publish(a.output_dir / "manifest.json", manifest)
                    usage, interrupted = reap(child, cancellation=cancellation)
                    row["active_pid"] = None
                    row.update(stopped=utc(), child_wall_seconds=time.monotonic() - begin)
                row.update(exit_code=child.returncode, peak_rss_native=usage.ru_maxrss,
                           peak_rss_bytes=usage.ru_maxrss * (1024 if sys.platform == "linux" else 1))
                if interrupted:
                    cancellation.check()
                    raise KeyboardInterrupt
                with report.open() as stream:
                    parsed = json.load(stream, parse_constant=reject_constant)
                if not isinstance(parsed, dict):
                    raise ValueError("Benchmark report must be an object")
                row["report_result"] = {k: parsed.get(k) for k in
                    ("status", "exit_code", "diagnostic", "source_identity", "requested_frames",
                     "requested_warmup", "completed_frames", "completed_warmup")}
                if child.returncode:
                    raise ValueError(f"Benchmark exited {child.returncode}")
                row["benchmark"] = inspect_report(parsed, expected, a.frames, a.warmup)
                row["complete"] = True
            finally:
                if child is not None and child.returncode is None:
                    usage, _ = reap(child, interrupted=True, cancellation=cancellation)
                    row.update(exit_code=child.returncode, peak_rss_native=usage.ru_maxrss,
                               peak_rss_bytes=usage.ru_maxrss * (1024 if sys.platform == "linux" else 1))
                row["active_pid"] = None
                if "stopped" not in row:
                    row.update(stopped=utc(), child_wall_seconds=time.monotonic() - begin)
                row["thermal_after_child"] = command_info(["/usr/bin/pmset", "-g", "therm"])
            publish(a.output_dir / "manifest.json", manifest)
            cancellation.check()
            print(f"[{index + 1}/{len(plan)}] {stem}: complete", flush=True)
        cancellation.check()
        manifest["complete"], code = True, 0
    except Cancelled as error:
        manifest["failure"] = (f"Cancelled by {signal.Signals(error.number).name}; "
                               "own benchmark child, if any, signaled and reaped")
        code = 128 + error.number
    except KeyboardInterrupt:
        manifest["failure"], code = "Interrupted; own benchmark child signaled and reaped", 130
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        manifest["failure"] = str(error)
    finally:
        try:
            if cancellation.number is not None:
                code = 128 + cancellation.number
                manifest["failure"] = (f"Cancelled by {signal.Signals(cancellation.number).name}; "
                                       "own benchmark child, if any, signaled and reaped")
            manifest["complete"] = code == 0 and len(manifest["runs"]) == len(plan) and all(
                row["complete"] for row in manifest["runs"])
            manifest.update(stopped=utc(), exit_code=code)
            publish(a.output_dir / "manifest.json", manifest)
            summary = {k: manifest[k] for k in ("schema", "complete", "requested_runs", "primary_reconstruction",
                                               "area_contract", "area_output", "area_target", "area_source_mis",
                                               "area_temporal_reuse", "started", "stopped", "exit_code")}
            summary["failure"] = manifest.get("failure")
            summary["runs"] = []
            for row in manifest["runs"]:
                summary["runs"].append({k: row.get(k) for k in
                    ("scene", "arm", "reconstruction", "seed", "complete", "exit_code", "child_wall_seconds",
                     "peak_rss_bytes", "child_pid", "active_pid", "report", "report_result")})
                benchmark = row.get("benchmark", {})
                summary["runs"][-1].update({k: benchmark.get(k) for k in ("summary", "pipeline_summary")})
            publish(a.output_dir / "summary.json", summary)
        finally:
            for number, handler in previous_handlers.items():
                signal.signal(number, handler)
    return code


if __name__ == "__main__":
    sys.exit(main())
