#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run table probes using already acquired, pinned PBRT references and asset cache.

No download is performed. Asset paths/hashes/terms are in the cache provenance.json.
The v3 harness links unmodified angular interpolation/BSDF sources; its local glog
shim supplies logging macros only. It uses RGBSpectrum to retain angular RGB.
Fourier GPU directions are checked by independent double CDF integrals; unmodified
v3 evaluates f/PDF at those identical directions. Its absolute-tolerance sampler
is intentionally not the oracle for the amplitude-normalized GPU inversion.
"""
import argparse
import hashlib
import json
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache/bsdf-tables"


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--v3", type=pathlib.Path, default=ROOT / ".cache/pbrt-v3-reference")
    parser.add_argument("--v4-build", type=pathlib.Path, default=ROOT / ".cache/pbrt-reference/build")
    args = parser.parse_args()
    CACHE.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((ROOT / "tests/fixtures/pbrt-render/surface-materials/external-tables.json").read_text())
    for asset in manifest["assets"]:
        path = CACHE / asset["file"]
        if path.stat().st_size != asset["bytes"] or hashlib.sha256(path.read_bytes()).hexdigest() != asset["sha256"]:
            raise RuntimeError(f"asset checksum mismatch: {path}")
    source = args.v4_build.parent / "source"
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if revision != manifest["pbrt_v4_source_revision"]:
        raise RuntimeError("v4 reference revision mismatch")
    run(["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Isrc",
         "tests/bsdf_table_tests.cpp", "src/bsdf_table.cpp", "-o", CACHE / "reader-tests"], cwd=ROOT)
    run([CACHE / "reader-tests"])
    run(["xcrun", "-sdk", "macosx", "metal", "-std=metal3.1", "-Wall", "-Werror", "-c",
         ROOT / "shaders/table_bsdf_probe.metal", "-o", CACHE / "probe.air"])
    run(["xcrun", "-sdk", "macosx", "metallib", CACHE / "probe.air", "-o", CACHE / "probe.metallib"])
    run(["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fobjc-arc", "-Isrc",
         "tests/table_bsdf_probe.mm", "src/bsdf_table.cpp", "-framework", "Metal", "-framework",
         "Foundation", "-o", CACHE / "gpu-probe"], cwd=ROOT)
    run([CACHE / "gpu-probe", CACHE / "probe.metallib"])
    revision = subprocess.check_output(["git", "-C", str(args.v3), "rev-parse", "HEAD"], text=True).strip()
    if revision != "13d871faae88233b327d04cda24022b8bb0093ee":
        raise RuntimeError("v3 reference revision mismatch")
    support = CACHE / "probe-support/glog"
    support.mkdir(parents=True, exist_ok=True)
    shim = "#pragma once\n#include <iostream>\n#define CHECK(x) if(x){}else std::cerr\n#define DCHECK(x) CHECK(x)\n"
    for name, op in [("EQ", "=="), ("NE", "!="), ("GT", ">"), ("GE", ">="), ("LT", "<"), ("LE", "<=")]:
        shim += f"#define CHECK_{name}(x,y) CHECK((x){op}(y))\n#define DCHECK_{name}(x,y) CHECK_{name}(x,y)\n"
    shim += "#define LOG(x) std::cerr\n#define VLOG(x) std::cerr\n"
    (support / "logging.h").write_text(shim)
    base = ["clang++", "-std=c++17", "-O2", "-ffunction-sections", "-fdata-sections",
            "-DPBRT_THREAD_LOCAL=thread_local", "-DPBRT_CONSTEXPR=constexpr", "-DPBRT_HAVE_ALLOCA_H",
            "-I" + str(support.parent), "-I" + str(args.v3 / "src"), "-I" + str(args.v3 / "src/core")]
    objects = []
    for file in ["core/reflection.cpp", "core/interpolation.cpp", "materials/fourier.cpp", "core/sampling.cpp"]:
        obj = CACHE / (pathlib.Path(file).stem + ".o")
        run(base + ["-c", args.v3 / "src" / file, "-o", obj])
        objects.append(obj)
    run(base + [ROOT / "scripts/pbrt_fourier_probe.cpp"] + objects + ["-Wl,-dead_strip", "-o", CACHE / "v3-oracle"])
    run([CACHE / "gpu-probe", "--write-fourier-fixture", CACHE / "synthetic-transmission.bsdf"])
    for name in ["roughgold_alpha_0.2", "paint", "synthetic-transmission"]:
        asset, oracle = CACHE / (name + ".bsdf"), CACHE / (name + "-oracle.txt")
        queries = CACHE / (name + "-queries.txt")
        run([CACHE / "gpu-probe", CACHE / "probe.metallib", asset, queries, "fourier-dump"])
        with oracle.open("w") as out:
            run([CACHE / "v3-oracle", asset, queries], stdout=out)
        run([CACHE / "gpu-probe", CACHE / "probe.metallib", asset, oracle, "fourier"])
    ninja = (args.v4_build / "build.ninja").read_text()
    includes = next(line.split("=", 1)[1] for line in ninja.splitlines()
                    if line.strip().startswith("INCLUDES =") and "source/src -" in line)
    run(["clang++", "-std=c++17", "-O2", "-ffp-contract=off"] + shlex.split(includes) +
        ["-c", ROOT / "scripts/pbrt_measured_probe.cpp", "-o", CACHE / "v4-oracle.o"], cwd=args.v4_build)
    start = ninja.index("build pbrt: ")
    block = ninja[start:ninja.index("\n\n", start)]
    libs = next(line.split("=", 1)[1] for line in block.splitlines() if line.strip().startswith("LINK_LIBRARIES ="))
    run(["clang++", CACHE / "v4-oracle.o", "-o", CACHE / "v4-oracle"] + shlex.split(libs), cwd=args.v4_build)
    for name in ["cm_white_spec", "aniso_brushed_aluminium_1_spec"]:
        asset, oracle = CACHE / (name + ".bsdf"), CACHE / (name + "-oracle.txt")
        with oracle.open("w") as out:
            run([CACHE / "v4-oracle", asset], stdout=out)
        run([CACHE / "gpu-probe", CACHE / "probe.metallib", asset, oracle])


if __name__ == "__main__":
    main()
