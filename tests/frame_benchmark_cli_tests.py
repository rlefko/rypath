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

invoke(["--help"], 0)
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
):
    invoke(args)

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
print("frame benchmark CLI checks passed")
