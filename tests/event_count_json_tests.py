#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Actual GPU-backed CLI serialization control for unsupported scalar step counts."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--allow-no-metal-device", action="store_true")
parser.add_argument("--allow-no-metal-ray-tracing", action="store_true")
args = parser.parse_args()
binary = str(args.binary.resolve())
with tempfile.TemporaryDirectory(prefix="rypath-event-count-json-") as directory:
    output = Path(directory) / "scalar.json"
    result = subprocess.run(
        [binary, "--demo", "--output", str(output), "--width", "2", "--height", "2",
         "--frames", "1", "--warmup", "0", "--profile-stages", "--execution", "wavefront",
         "--spectral-transport", "scalar"],
        capture_output=True, text=True,
    )
    report = json.loads(output.read_text())
    if result.returncode == 2:
        diagnostic = report["diagnostic"]
        permitted = (
            args.allow_no_metal_device and diagnostic == "No system-default Metal device"
        ) or (
            args.allow_no_metal_ray_tracing
            and diagnostic == "System-default Metal device does not support compute ray tracing"
        )
        assert permitted, (result.returncode, report, result.stderr)
        assert report["status"] == "unavailable" and report["exit_code"] == 2, report
        assert report["frames"] == [] and report["completed_frames"] == 0, report
        assert report["completed_warmup"] == 0, report
        assert result.stderr.splitlines().count("frame benchmark: " + diagnostic) == 1, result.stderr
        assert not result.stdout.strip(), result.stdout
        print("SKIP: configured unavailable Metal capability: " + diagnostic)
        sys.exit(77)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert report["settings"]["profile_stages"] is True
    assert len(report["frames"]) == 1
    work = report["frames"][0]["path_work"]
    assert work["requested"] is True and work["complete"] is True, work
    assert work["packet_step_invocations_supported"] is False, work
    assert work["passes"], work
    assert all(row["packet_step_invocations"] is None for row in work["passes"]), work
