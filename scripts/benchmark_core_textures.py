#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run matched macOS Metal reference renders and retain timing and raw PFM evidence."""

import argparse
import hashlib
import itertools
import json
import math
import re
import shutil
import statistics
import subprocess
import time
from pathlib import Path

from compare_pbrt import read_pfm, stage_dependencies


FIXTURES = Path(__file__).resolve().parents[1] / "tests/fixtures/pbrt-render"
REFLECTANCE = 'Material "diffuse" "spectrum reflectance" [360 0.1 476 0.8 503.5 0.2 575 0.9 830 0.3]'
DIRECT = 'Material "diffuse" "rgb reflectance" [0.375 0.375 0.375]'
GRAPH = (
    'Texture "blend" "spectrum" "mix" "rgb tex1" [0.25 0.25 0.25]\n'
    '    "rgb tex2" [0.5 0.5 0.5] "float amount" [0.5]\n'
    'Material "diffuse" "texture reflectance" "blend"'
)
ALPHA = (
    'Texture "mask" "float" "imagemap" "string filename" "assets/white.png"\n'
    '    "string filter" "point"'
)


def render_settings(source, size, samples):
    for name, value in (("xresolution", size), ("yresolution", size), ("pixelsamples", samples)):
        source, count = re.subn(rf'("integer {name}"\s+)\[\s*\d+\s*\]', rf'\g<1>[{value}]', source)
        if count != 1:
            raise ValueError(f"expected one {name} setting")
    return source


def scenarios(baseline, candidate, work, sample_scale):
    for name, size, samples in (
        ("diffuse", 64, 4096),
        ("image-ewa", 8, 131072),
        ("cornell-box-adapted", 64, 2048),
    ):
        directory = work / name
        directory.mkdir()
        fixture = FIXTURES / f"{name}.pbrt"
        stage_dependencies(fixture, directory)
        source = fixture.read_text()
        yield directory, size, samples * sample_scale, ((baseline, source), (candidate, source))

    source = (FIXTURES / "diffuse.pbrt").read_text().replace(REFLECTANCE, DIRECT)
    if DIRECT not in source:
        raise ValueError("diffuse fixture reflectance changed")
    graph_dir = work / "mix-vs-direct"
    graph_dir.mkdir()
    graph = source.replace(DIRECT, GRAPH)
    yield graph_dir, 64, 4096 * sample_scale, ((candidate, source), (candidate, graph))

    alpha_dir = work / "image-alpha-vs-opaque"
    assets = alpha_dir / "assets"
    assets.mkdir(parents=True)
    shutil.copyfile(FIXTURES / "core-textures/assets/white.png", assets / "white.png")
    opaque = source.rstrip() + '\n    "point2 uv" [0 0 1 0 1 1 0 1]\n'
    masked = opaque.replace(DIRECT, f"{ALPHA}\n{DIRECT}") + '    "texture alpha" "mask"\n'
    yield alpha_dir, 64, 4096 * sample_scale, ((candidate, opaque), (candidate, masked))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def render(binary, scene, output):
    begin = time.perf_counter()
    result = subprocess.run(
        ["/usr/bin/time", "-l", str(binary), str(scene), str(output)],
        cwd=scene.parent,
        capture_output=True,
        text=True,
        timeout=900,
    )
    elapsed = time.perf_counter() - begin
    if result.returncode != 0:
        raise RuntimeError(f"{scene}: {result.stderr}")
    output.with_suffix(".log").write_text(result.stdout + result.stderr)
    peak = re.search(r"^\s*(\d+)\s+maximum resident set size$", result.stderr, re.MULTILINE)
    if peak is None:
        raise RuntimeError(f"{scene}: no maximum resident set size from macOS time -l")
    return {"seconds": elapsed, "rss_bytes": int(peak[1]), "sha256": digest(output)}


def compare(left, right, size):
    left_channels = itertools.chain.from_iterable(read_pfm(left, size, size))
    right_channels = itertools.chain.from_iterable(read_pfm(right, size, size))
    differences = [abs(a - b) for a, b in zip(left_channels, right_channels, strict=True)]
    return {
        "max_absolute": max(differences),
        "rmse": math.sqrt(statistics.mean(delta * delta for delta in differences)),
    }


def benchmark(directory, size, samples, variants, repeats):
    for side, (_, source) in enumerate(variants):
        (directory / f"{side}.pbrt").write_text(render_settings(source, size, samples))
        (directory / f"{side}-startup.pbrt").write_text(render_settings(source, 1, 1))
    results = {
        "size": size,
        "samples": samples,
        "scene_sha256": [digest(directory / f"{side}.pbrt") for side in range(2)],
        "startup": [[], []],
        "full": [[], []],
    }
    for series, label, count in (("startup", "-startup", 3), ("full", "", repeats + 1)):
        for round_index in range(count):
            order = (0, 1) if round_index % 2 == 0 else (1, 0)
            for side in order:
                tag = f"{side}{label}"
                result = render(
                    variants[side][0],
                    directory / f"{tag}.pbrt",
                    directory / f"{tag}-{round_index}.pfm",
                )
                results[series][side].append(result)
    results["raw_difference"] = compare(directory / "0-1.pfm", directory / "1-1.pfm", size)
    results["repeatable"] = [
        len({result["sha256"] for result in side}) == 1 for side in results["full"]
    ]
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True, help="new directory for rendered outputs")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--sample-scale", type=int, default=1)
    args = parser.parse_args()
    if args.repeats < 3 or args.sample_scale < 1:
        parser.error("use at least three repeats and a positive sample scale")
    work = args.work_dir.resolve()
    work.mkdir(parents=True)
    baseline, candidate = args.baseline.resolve(), args.candidate.resolve()
    report = {}
    for label, binary in (("baseline", baseline), ("candidate", candidate)):
        report[label] = {
            "binary_sha256": digest(binary),
            "metallib_sha256": digest(binary.with_name("rypath.metallib")),
        }
    report["scenarios"] = {}
    for directory, size, samples, variants in scenarios(baseline, candidate, work, args.sample_scale):
        result = benchmark(directory, size, samples, variants, args.repeats)
        report["scenarios"][directory.name] = result
        (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(directory.name, json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
