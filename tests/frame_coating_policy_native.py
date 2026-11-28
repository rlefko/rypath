#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Strict native policy/provenance differential; caller owns the GPU lease.

Requires NumPy/OpenEXR. Uses fresh output directories and never retries failures.
The legacy executable and metallib must be frozen from the matched base revision.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

import numpy as np
import OpenEXR


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_completed_provenance(report, directory, effective):
    """Audit stored completed records without rendering again."""
    shared = effective == "shared-diffuse"
    source = "explicit-frame-session-v1" if shared else "legacy-default"
    assert report["settings"]["coating_proposal_source"] == source
    assert len(report["frames"]) == report["completed_frames"]
    for row in report["frames"]:
        assert row["complete"]
        assert row["coating_proposal"] == effective
        assert row["coating_proposal_contract"] == int(shared)
        assert row["coating_proposal_capability_verified"] == shared
    for label in ["raw", "signed"]:
        with OpenEXR.File(str(directory / (label + ".exr"))) as f:
            h = f.header()
            assert h["rypath.frame.coating_proposal_source"] == source
            assert h["rypath.frame.source_identity"] == report["source_identity"]
            assert h["rypath.frame.physical_source_identity"] == report["physical_source_identity"]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ["benchmark", "legacy-benchmark", "old-metallib", "work-dir", "scene"]:
        p.add_argument("--" + key, type=Path, required=True)
    a = p.parse_args()
    a.work_dir.mkdir(parents=True, exist_ok=False)
    inputs = [a.benchmark, a.benchmark.parent / "rypath.metallib", a.legacy_benchmark,
              a.old_metallib, a.scene]
    binding = {str(x.resolve()): sha(x) for x in inputs}
    (a.work_dir / "protocol.json").write_text(json.dumps({
        "inputs": binding, "seed": 702211, "frames": 2,
        "checks": "exact default/off/legacy pixels; proposal changes supported fixture; "
                  "actual raw/reconstruction/report policy; old capability rejects opt-in",
        "failure_policy": "stop at first failure; no retries or exclusions",
    }, indent=2) + "\n")
    hosts = {}
    for name, host, lib in [("new", a.benchmark, inputs[1]),
                            ("new-oldlib", a.benchmark, a.old_metallib),
                            ("legacy", a.legacy_benchmark, a.old_metallib)]:
        d = a.work_dir / name
        d.mkdir()
        os.link(host, d / "rypath-frame-benchmark")
        os.link(lib, d / "rypath.metallib")
        hosts[name] = d / "rypath-frame-benchmark"

    def run(name, host="new", policy=None, area=None, reject=False):
        d = a.work_dir / name
        d.mkdir()
        cmd = [str(hosts[host].resolve()), "--scene", str(a.scene.resolve()),
               "--output", str(d / "report.json"), "--capture", str(d / "raw.exr"),
               "--reconstructed-capture", str(d / "signed.exr"), "--reconstruction", "temporal",
               "--width", "16", "--height", "16", "--output-width", "16", "--output-height", "16",
               "--seed", "702211", "--warmup", "0", "--frames", "2",
               "--spectral-transport", "hero", "--diffuse-events", "generic"]
        if policy:
            cmd += ["--coating-proposal", policy]
        if area:
            cmd += ["--estimator", "area-replay", "--area-path-domain", "enhanced",
                    "--area-contribution-replay", "prefix", "--area-reuse", area, "--mutations", "0"]
        (d / "command.json").write_text(json.dumps(cmd, indent=2) + "\n")
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=180, env=dict(os.environ, MTL_DEBUG_LAYER="1"))
        (d / "process.log").write_bytes(result.stdout)
        if reject:
            assert result.returncode != 0
            assert b"lacks shared coating proposal capability v1" in result.stdout
            return None
        assert result.returncode == 0, (name, result.returncode, result.stdout[-4000:])
        report = json.loads((d / "report.json").read_text())
        assert report["status"] == "complete" and report["completed_frames"] == 2
        images = []
        for label in ["raw", "signed"]:
            with OpenEXR.File(str(d / (label + ".exr")), separate_channels=True) as f:
                image = np.stack([f.channels()[k].pixels for k in "RGB"], axis=-1)
                assert np.isfinite(image).all()
                images.append(image.copy())
                if host != "legacy":
                    effective = policy or "hero-only"
                    h = f.header()
                    assert h["rypath.frame.coating_proposal"] == effective
                    assert h["rypath.frame.coating_proposal_requested"] == effective
                    assert h["rypath.frame.coating_proposal_contract"] == ("1" if policy == "shared-diffuse" else "0")
                    assert h["rypath.frame.coating_proposal_capability_verified"] == ("true" if policy == "shared-diffuse" else "false")
        if host != "legacy":
            s = report["settings"]
            effective = policy or "hero-only"
            assert s["coating_proposal_effective"] == effective
            assert s["coating_proposal_requested"] == effective
            assert s["coating_proposal_capability_verified"] == (policy == "shared-diffuse")
            assert s["coating_proposal_contract"] == (1 if policy == "shared-diffuse" else 0)
            assert (report["source_identity"] != report["physical_source_identity"]) == (policy == "shared-diffuse")
            verify_completed_provenance(report, d, effective)
        return images

    legacy = run("legacy-default", "legacy")
    oldlib = run("new-oldlib-default", "new-oldlib")
    default = run("new-default")
    off = run("new-explicit-off", policy="hero-only")
    for images in [oldlib, default, off]:
        assert all(np.array_equal(x, y) for x, y in zip(legacy, images)), "legacy pixels changed"
    on = run("new-on", policy="shared-diffuse")
    assert not np.array_equal(on[0], off[0]), "fixture did not exercise changed proposal"
    run("enhanced-full", policy="shared-diffuse", area="full")
    run("enhanced-fresh", policy="shared-diffuse", area="fresh")
    run("old-capability-rejection", "new-oldlib", policy="shared-diffuse", reject=True)
    assert all(sha(Path(x)) == digest for x, digest in binding.items())
    (a.work_dir / "complete.json").write_text(json.dumps({"complete": True, "runs": 8}) + "\n")
    print("8 native coating policy/default/provenance controls passed")


if __name__ == "__main__":
    main()
