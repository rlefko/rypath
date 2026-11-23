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

help_text = invoke(["--help"], 0).stdout
assert "--intersections native|compensated" in help_text
assert "--shadow-traversal query|opaque-auto" in help_text
assert "--as-build default|fast-blas|fast-tlas|fast-all" in help_text
assert "--reconstruction none|raw-rgb|metalfx|temporal" in help_text
assert "--guide-cache on|off" in help_text
assert "--reconstructed-capture" in help_text
assert "--profile-stages" in help_text
assert "--capture-frame" in help_text and "--capture-path" in help_text
for args in (
    ["--demo", "--output", "x.json", "--capture-frame", "0"],
    ["--demo", "--output", "x.json", "--capture-path", "x.gputrace"],
    ["--demo", "--output", "x.json", "--capture-frame", "-1", "--capture-path", "x.gputrace"],
    ["--demo", "--output", "x.json", "--frames", "1", "--warmup", "0", "--capture-frame", "1", "--capture-path", "x.gputrace"],
    ["--demo", "--output", "x.json", "--capture-frame", "0", "--capture-path", "x.json"],
    ["--demo", "--output", "x.json", "--as-build"],
    ["--demo", "--output", "x.json", "--as-build", "invalid"],
    ["--demo", "--output", "x.json", "--as-build", "fast-all", "--intersections", "compensated"],
    ["--demo", "--output", "x.json", "--execution", "megakernel", "--packet-events", "2"],
    ["--demo", "--output", "x.json", "--execution", "megakernel", "--packet-layout", "hot-cold"],
    ["--demo", "--output", "x.json", "--packet-layout", "split-trace", "--packet-events", "4"],
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
    ("--packet-events", ("0", "3", "5", "-1", "nan")),
    ("--packet-layout", ("invalid",)),
    ("--execution", ("invalid",)),
    ("--estimator", ("invalid",)),
    ("--mutations", ("3", "5", "-1", "nan", "4294967296", "1x")),
    ("--target-scale", ("0", "-1", "nan", "inf", "1e-45", "1e100", "1e-6x")),
    ("--intersections", ("invalid",)),
    ("--motion-offset", ("0", "nan", "inf", "1e100", "1x")),
    ("--shadow-traversal", ("invalid",)),
    ("--capacity", ("0", "65537", "-1")),
    ("--quantum", ("0", "1025", "-1")),
    ("--group-width", ("0", "257", "-1")),
):
    for value in values:
        invoke(["--demo", "--output", "x.json", option, value])

for extra in (
    ["--guide-cache"],
    ["--guide-cache", "invalid"],
    ["--guide-cache", "on"],
    ["--reconstruction", "none", "--guide-cache", "on"],
    ["--reconstruction", "raw-rgb", "--guide-cache", "on"],
    ["--reconstruction", "temporal", "--guide-cache", "off", "--guide-cache", "on"],
    ["--reconstruction"],
    ["--reconstruction", "invalid"],
    ["--reconstruction", "none", "--reconstruction", "raw-rgb"],
    ["--output-width", "1920"],
    ["--output-height", "1080"],
    ["--reconstruction", "none", "--output-width", "1920"],
    ["--reconstructed-capture", "image.exr"],
    ["--reconstruction", "raw-rgb", "--reconstructed-capture", "image.png"],
    ["--reconstruction", "raw-rgb", "--output-width", "1921"],
    ["--reconstruction", "raw-rgb", "--output-height", "1079"],
    ["--reconstruction", "temporal", "--output-width", "1921"],
    ["--reconstruction", "temporal", "--output-height", "1079"],
    ["--reconstruction", "metalfx", "--output-width", "1919"],
    ["--reconstruction", "metalfx", "--output-height", "1079"],
    ["--reconstruction", "raw-rgb", "--width", "16385"],
    ["--reconstruction", "metalfx", "--height", "16385"],
    ["--reconstruction", "metalfx", "--output-width", "16385"],
    ["--reconstruction", "metalfx", "--output-height", "16385"],
):
    invoke(["--demo", "--output", "x.json", *extra])
for key in ("--output-width", "--output-height"):
    for value in ("0", "-1", "nan", "4294967296", "1x"):
        invoke(["--demo", "--output", "x.json", "--reconstruction", "metalfx", key, value])

invoke(["--demo", "--output", "x.json", "--estimator", "area-replay", "--execution", "megakernel"])
for frames in ("4", "7", "9"):
    invoke(["--demo", "--output", "x.json", "--motion-offset", ".1", "--frames", frames])

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
    # All reconstructed destinations and their pending names participate in
    # reservation conflicts, including aliases of a checked source dependency.
    reconstructed_alias = root / "source-alias.exr"
    reconstructed_alias.symlink_to(child)
    destination = root / "reconstruction-alias.json"
    invoke(["--scene", str(scene), "--output", str(destination), "--reconstruction", "raw-rgb",
            "--reconstructed-capture", str(reconstructed_alias)])
    assert not destination.exists() and child.read_bytes() == original[child]
    duplicate = root / "duplicate.exr"
    invoke(["--demo", "--output", str(duplicate), "--reconstruction", "raw-rgb",
            "--reconstructed-capture", str(duplicate)])
    assert not duplicate.exists()
    overlap = root / "overlap.exr"
    invoke(["--demo", "--output", str(overlap) + ".pending", "--reconstruction", "raw-rgb",
            "--reconstructed-capture", str(overlap)])
    assert not overlap.exists() and not Path(str(overlap) + ".pending").exists()
    locked_capture = root / "locked.exr"
    Path(str(locked_capture) + ".pending").write_text("keep reconstructed pending")
    destination = root / "locked-report.json"
    invoke(["--scene", str(scene), "--output", str(destination), "--reconstruction", "raw-rgb",
            "--reconstructed-capture", str(locked_capture)])
    assert not destination.exists() and not locked_capture.exists()
    assert Path(str(locked_capture) + ".pending").read_text() == "keep reconstructed pending"
    for path, content in original.items():
        assert path.read_bytes() == content
    trace = root / "existing.gputrace"
    trace.mkdir()
    invoke(["--demo", "--output", str(root / "trace-report.json"),
            "--capture-frame", "0", "--capture-path", str(trace)])
    assert trace.is_dir() and not (root / "trace-report.json").exists()
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
    assert data["settings"]["reconstruction"] == "none"
    assert data["settings"]["guide_cache"] is False
    assert (data["settings"]["output_width"], data["settings"]["output_height"]) == (1920, 1080)
    assert data["pipeline_summary"]["completed_frames"] == 0
    assert data["pipeline_summary"]["frames_per_second"] is None
    assert data["reconstruction_startup_seconds"] == 0
    assert "startup GPU probe" in data["pipeline_GPU_scope"]
    for mode in ("none", "raw-rgb", "metalfx", "temporal"):
        destination = root / ("reconstruction-" + mode + ".json")
        extra = [] if mode == "none" else ["--output-width", "17", "--output-height", "9"]
        invoke(["--scene", str(malformed), "--output", str(destination), "--width", "17", "--height", "9",
                "--reconstruction", mode, *extra])
        selected = json.loads(destination.read_text())
        settings = selected["settings"]
        assert settings["reconstruction"] == mode
        assert settings["guide_cache"] is (mode in ("metalfx", "temporal"))
        assert (settings["internal_width"], settings["internal_height"]) == (17, 9)
        assert (settings["output_width"], settings["output_height"]) == (17, 9)
        assert settings["output_pixels_per_input_pixel"] == 1
        assert "reconstruction mode " + mode in selected["provenance"]
        assert selected["frames"] == [] and selected["pipeline_summary"]["GPU"]["count"] == 0
        assert selected["pipeline_summary"]["cpu_wall"]["count"] == 0
    for mode in ("none", "raw-rgb", "metalfx", "temporal"):
        choices = ("off", "on") if mode in ("metalfx", "temporal") else ("off",)
        for choice in choices:
            destination = root / f"guide-cache-{mode}-{choice}.json"
            # An explicit setting before the reconstruction option must win over
            # mode-dependent defaults resolved after the complete argument list.
            invoke(["--scene", str(malformed), "--output", str(destination),
                    "--guide-cache", choice, "--reconstruction", mode])
            settings = json.loads(destination.read_text())["settings"]
            assert settings["reconstruction"] == mode
            assert settings["guide_cache"] is (choice == "on")
    destination = root / "profile-options.json"
    trace = root / "fresh.gputrace"
    invoke(["--scene", str(malformed), "--output", str(destination),
            "--profile-stages", "--capture-frame", "0", "--capture-path", str(trace)])
    settings = json.loads(destination.read_text())["settings"]
    assert settings["profile_stages"] is True
    assert settings["capture_frame"] == 0 and settings["capture_path"] == str(trace)
    assert not trace.exists()
    destination = root / "guide-cache.json"
    invoke(["--scene", str(malformed), "--output", str(destination),
            "--reconstruction", "temporal", "--guide-cache", "on", "--profile-stages"])
    assert json.loads(destination.read_text())["settings"]["guide_cache"] is True
    assert json.loads(destination.read_text())["settings"]["profile_stages"] is True
    for policy in ("default", "fast-blas", "fast-tlas", "fast-all"):
        destination = root / ("as-build-" + policy + ".json")
        invoke(["--scene", str(malformed), "--output", str(destination), "--as-build", policy,
                "--shadow-traversal", "opaque-auto", "--profile-stages"])
        selected = json.loads(destination.read_text())
        assert selected["settings"]["as_build"] == policy
        assert selected["settings"]["shadow_traversal_requested"] == "opaque-auto"
        assert selected["settings"]["profile_stages"] is True
        metadata = selected["acceleration_build"]
        assert metadata["requested"] == policy and metadata["effective"] is None
        assert metadata["supported"] is None
        assert metadata["blas_bytes"] == metadata["tlas_bytes"] == 0
        assert metadata["device_allocated_bytes"] is None
    for layout in ("packed", "hot-cold", "split-trace"):
        destination = root / ("packet-" + layout + ".json")
        invoke(["--scene", str(malformed), "--output", str(destination), "--packet-layout", layout])
        selected = json.loads(destination.read_text())["settings"]
        assert selected["packet_layout"] == layout and selected["packet_events"] == 1
        assert selected["effective_packet_events"] is None and selected["effective_packet_layout"] is None
    destination = root / "upscale.json"
    invoke(["--scene", str(malformed), "--output", str(destination), "--width", "17", "--height", "9",
            "--reconstruction", "metalfx", "--output-width", "34", "--output-height", "18"])
    settings = json.loads(destination.read_text())["settings"]
    assert (settings["width"], settings["height"]) == (17, 9)
    assert (settings["output_width"], settings["output_height"]) == (34, 18)
    assert settings["output_pixels_per_input_pixel"] == 4
    destination = root / "width-only.json"
    invoke(["--scene", str(malformed), "--output", str(destination), "--width", "17", "--height", "9",
            "--reconstruction", "metalfx", "--output-width", "34"])
    settings = json.loads(destination.read_text())["settings"]
    assert settings["output_height"] == 9

    # A malformed input stops before GPU setup while preserving parsed profile settings.
    for profile in ("native", "compensated"):
        selected = root / f"{profile}.json"
        invoke(["--scene", str(malformed), "--output", str(selected), "--intersections", profile])
        selected_data = json.loads(selected.read_text())
        assert selected_data["settings"]["intersection_profile"] == profile
        assert selected_data["completed_frames"] == 0
    for mode in ("query", "opaque-auto"):
        destination = root / f"shadow-{mode}.json"
        invoke(["--scene", str(malformed), "--output", str(destination),
                "--shadow-traversal", mode])
        selected = json.loads(destination.read_text())
        assert selected["settings"]["shadow_traversal_requested"] == mode
        assert selected["settings"]["shadow_traversal_effective"] is None
        assert selected["shadow_traversal_frames"] == {
            "opaque_auto": 0, "query": 0, "excludes_warmup": True
        }
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
