#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""GPU-independent argument and input/output safety checks."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())

def invoke(args, expected=1):
    result = subprocess.run([binary, *args], capture_output=True, text=True)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result

assert "--intersections native|compensated" in invoke(["--help"], 0).stdout
for args in (
    [], ["--demo"], ["--demo", "--scene", "x", "--output", "x.json"],
    ["--demo", "--demo", "--output", "x.json"],
    ["--demo", "--output", "x.json", "--width", "0"],
    ["--demo", "--output", "x.json", "--frames", "-1"],
    ["--demo", "--output", "x.json", "--frames", "18446744073709551616"],
    ["--demo", "--output", "x.json", "--frames", "18446744073709551615", "--warmup", "1"],
    ["--demo", "--output", "x.json", "--width", "65536", "--height", "65536"],
    ["--demo", "--output", "x.json", "--material-seed", "2147483648"],
    ["--demo", "--output", "x.json", "--capture", "x.png"],
    ["--demo", "--output", "x.json", "--preview", "x.exr"],
    ["--demo", "--output", "x.json", "--unknown", "1"],
    ["--demo", "--output", "x.json", "--intersections"],
    ["--demo", "--output", "x.json", "--intersections", "fast"],
    ["--demo", "--output", "x.json", "--intersections", "native", "--intersections", "compensated"],
):
    invoke(args)

for option, values in (
    ("--execution", ("invalid",)),
    ("--estimator", ("invalid",)),
    ("--mutations", ("3", "5", "-1", "nan", "4294967296", "1x")),
    ("--target-scale", ("0", "-1", "nan", "inf", "1e-45", "1e100", "1e-6x")),
    ("--intersections", ("invalid",)),
    ("--capacity", ("0", "65537", "-1")),
    ("--quantum", ("0", "1025", "-1")),
    ("--group-width", ("0", "257", "-1")),
):
    for value in values:
        invoke(["--demo", "--output", "x.json", option, value])

invoke(["--demo", "--output", "x.json", "--estimator", "area-replay", "--execution", "megakernel"])

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    scene = root / "scene.pbrt"
    child = root / "child.pbrt"
    scene.write_text('Include "child.pbrt"\nWorldBegin\n')
    child.write_text('# input dependency\n')
    original = {scene: scene.read_bytes(), child: child.read_bytes()}
    for output in (scene, child, root / ".." / root.name / "child.pbrt"):
        invoke(["--scene", str(scene), "--output", str(output)])
    alias = root / "alias.json"
    alias.symlink_to(child)
    invoke(["--scene", str(scene), "--output", str(alias)])
    report = root / "report.json"
    invoke(["--demo", "--output", str(report), "--capture", str(report)])
    assert not report.exists()
    pending = root / "fresh.json.pending"
    pending.write_text("keep pending")
    invoke(["--demo", "--output", str(root / "fresh.json")])
    assert pending.read_text() == "keep pending"
    assert not (root / "fresh.json").exists()
    for path, content in original.items():
        assert path.read_bytes() == content
    # Startup failures retain a complete JSON report without GPU work.
    malformed = root / "broken.pbrt"
    malformed.write_text('ThisIsNotPBRT\n')
    invoke(["--scene", str(malformed), "--output", str(report)])
    data = json.loads(report.read_text())
    assert data["status"] == "failed" and data["exit_code"] == 1
    assert data["completed_frames"] == data["completed_warmup"] == 0
    assert data["requested_frames"] == 120 and data["requested_warmup"] == 8
    assert data["frames"] == [] and data["consumed_sample_indices"] == 0
    assert data["summary"]["percentile_method"] == "nearest-rank"
    for metric in ("GPU", "cpu_wall"):
        assert data["summary"][metric] == {
            "count": 0, "median_seconds": None, "p95_seconds": None
        }
    assert data["summary"]["missing_GPU_timestamps"] == 0
    assert data["capture_in_frame_timing"] is False
    assert data["settings"]["execution"] == "wavefront"
    assert data["settings"]["estimator"] == "independent"
    assert data["settings"]["replay_contract"] == 0
    for execution in ("wavefront", "megakernel"):
        destination = root / (execution + ".json")
        invoke(["--scene", str(malformed), "--output", str(destination),
                "--execution", execution, "--capacity", "17", "--quantum", "1",
                "--group-width", "64", "--intersections", "compensated"])
        settings = json.loads(destination.read_text())["settings"]
        assert settings["execution"] == execution
        assert (settings["capacity"], settings["quantum"], settings["group_width"]) == (17, 1, 64)
        assert settings["effective_capacity"] == (17 if execution == "wavefront" else None)
        assert settings["intersection_profile"] == "compensated"
    assert data["settings"]["intersection_profile"] == "native"
    # A malformed input stops before GPU setup while preserving parsed profile settings.
    for profile in ("native", "compensated"):
        selected = root / f"{profile}.json"
        invoke(["--scene", str(malformed), "--output", str(selected), "--intersections", profile])
        selected_data = json.loads(selected.read_text())
        assert selected_data["settings"]["intersection_profile"] == profile
        assert selected_data["completed_frames"] == 0
    destination = root / "area.json"
    invoke(["--scene", str(malformed), "--output", str(destination),
            "--estimator", "area-replay", "--target-scale", "0.125"])
    settings = json.loads(destination.read_text())["settings"]
    assert settings["estimator"] == "area-replay" and settings["replay_contract"] == 1
    assert settings["area_contract"] == 3
    assert settings["mutation_steps"] == 1 and settings["effective_mutation_steps"] == 1
    assert settings["area_target"] == "bounded-l1-contrast-0.1-v1"
    assert settings["target_scale"] == 0.125 and settings["fresh_paths_per_pixel_per_frame"] == 1
    for count in (0, 1, 2, 4):
        destination = root / f"area-mutations-{count}.json"
        invoke(["--scene", str(malformed), "--output", str(destination),
                "--estimator", "area-replay", "--mutations", str(count)])
        settings = json.loads(destination.read_text())["settings"]
        assert settings["mutation_steps"] == count and settings["effective_mutation_steps"] == count
print("frame benchmark CLI checks passed")
