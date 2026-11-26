#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Retain a failed hosted probe and run distinct, non-gating diagnostics.

This collector never retries the combined test or classifies it as a skip/pass.
The workflow retains the original CTest failure independently of this command.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


LOG_LIMIT = 1024 * 1024
ARTIFACT_LIMIT = 256 * 1024 * 1024


def capture(command, destination, timeout=30, limit=LOG_LIMIT, env=None):
    """Record the real exit/timeout and retain at most limit output bytes."""
    started = time.monotonic()
    result = {"command": [str(part) for part in command], "timeout_seconds": timeout}
    try:
        completed = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False, env=env,
        )
        output = completed.stdout
        result.update(exit_code=completed.returncode, timed_out=False)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b""
        result.update(exit_code=None, timed_out=True)
    except OSError as error:
        output = str(error).encode("utf-8", errors="replace")
        result.update(exit_code=None, timed_out=False, launch_error=str(error))
    destination.write_bytes(output[:limit])
    result.update(
        log=destination.name, elapsed_seconds=time.monotonic() - started,
        output_bytes=len(output), retained_bytes=min(len(output), limit),
        truncated=len(output) > limit,
    )
    return result


def retain(source, destination):
    if not source.is_file():
        return {"source": str(source), "status": "missing"}
    size = source.stat().st_size
    if size > ARTIFACT_LIMIT:
        return {"source": str(source), "status": "too_large", "bytes": size}
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    digest = hashlib.sha256()
    with destination.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"source": str(source), "status": "retained", "bytes": size,
            "sha256": digest.hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest = {"schema": 1, "status": "diagnostic_only", "collection_complete": False,
                "started_utc": datetime.now(timezone.utc).isoformat(),
                "original_test_result": "unchanged; see retained CTest logs",
                "artifacts": {}, "commands": []}
    path = output / "manifest.json"

    def publish():
        path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    # Explicit allowlist: no workspace archive, environment dump or credentials.
    files = [
        "rypath_hero_current_probe", "rypath-hero-current-probe.metallib",
        "CMakeFiles/rypath-hero-current-probe.air",
        "CMakeFiles/hero-current-probe/generation.json",
        "CMakeFiles/hero-current-probe/current_probe.metal",
        "CMakeFiles/hero-current-probe/current_render.metal",
        "CMakeFiles/hero-current-probe/current_packet_transport.h",
        "CMakeFiles/hero-current-probe/current_packet_pt_state.h",
        "Testing/Temporary/LastTest.log", "Testing/Temporary/LastTestsFailed.log",
    ]
    for name in files:
        manifest["artifacts"][name] = retain(build / name, output / "artifacts" / name)
    publish()
    commands = [
        ("source-revision", ["git", "rev-parse", "HEAD"], 15),
        ("os", ["sw_vers"], 15),
        ("xcode", ["xcodebuild", "-version"], 15),
        ("metal-version", ["xcrun", "--sdk", "macosx", "metal", "--version"], 15),
        ("build-commands", ["ninja", "-C", str(build), "-t", "commands",
                            "rypath_hero_current_shader"], 15),
        ("compiler-log", ["/usr/bin/log", "show", "--style", "compact", "--last", "3m",
                          "--predicate", 'process CONTAINS[c] "MTLCompiler" OR '
                          'process CONTAINS[c] "MetalCompiler"'], 20),
    ]
    for name, command, timeout in commands:
        manifest["commands"].append(capture(command, output / f"{name}.log", timeout))
        publish()
    executable = build / "rypath_hero_current_probe"
    library = build / "rypath-hero-current-probe.metallib"
    environment = os.environ.copy()
    environment["MTL_DEBUG_LAYER"] = "1"
    # Each mode gets a fresh process and the SAME retained build artifacts.
    # Neither reruns or replaces the failing default combined CTest invocation.
    for mode in ("preparation-only", "sampling-only"):
        record = capture([str(executable), f"--{mode}", str(library)],
                         output / f"{mode}.log", 120, env=environment)
        record["scope"] = "diagnostic, not a replacement validation result"
        manifest["commands"].append(record)
        publish()
    manifest["collection_complete"] = True
    manifest["completed_utc"] = datetime.now(timezone.utc).isoformat()
    publish()
    print(f"Hero pipeline diagnostics retained in {output}; original test result unchanged")


if __name__ == "__main__":
    main()
