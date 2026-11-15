#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Independent installed OpenEXR readback, pinned PBRT source/mips and HDR renders.

Uses existing comparison seeds, sample budgets and thresholds without tuning.
Installed OpenEXR is an oracle dependency only. All generated work stays in output-dir.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile

import compare_pbrt as comparison
from extract_pbrt_rgb import REVISION

SCENES = ("hdr-half-diffuse", "hdr-float-graph", "hdr-rgbe-diffuse", "hdr-area",
          "hdr-environment", "hdr-external")
ROOT = Path(__file__).resolve().parents[1]
EXTERNAL = {"path": "assets/external-comp-zip.exr", "bytes": 1180708,
            "sha256": "b51491f94c30896e038ab13286922d75f36f65a42533a8c9acac72b81b1a84a8"}


def run(command, **kwargs):
    return subprocess.check_output([str(x) for x in command], text=True, **kwargs)


def build_probe(build, target, source, destination, define):
    """Reuse the supplied build's compiler and linker flags, without changing its graph."""
    commands = run(["ninja", "-C", build, "-t", "commands", target]).splitlines()
    link = shlex.split(commands[-1].split("&&")[1])
    original_object = next(x for x in link if x.endswith(".o"))
    compile_line = next(x for x in commands if " -c " in x and original_object in x)
    args = shlex.split(compile_line)
    filtered = []
    i = 0
    while i < len(args):
        if args[i] in ("-MT", "-MF", "-o", "-c"):
            i += 2
        elif args[i] == "-MD":
            i += 1
        else:
            filtered.append(args[i]); i += 1
    obj = destination.with_suffix(".o")
    subprocess.run([*filtered, f"-D{define}", "-c", source, "-o", obj], check=True, cwd=build)
    link[link.index(original_object)] = str(obj)
    link[link.index("-o") + 1] = str(destination)
    subprocess.run(link, check=True, cwd=build)


def probes(args, work):
    source = ROOT / "scripts/hdr_io_reference.cpp"
    sdk = work / "installed-openexr"
    flags = shlex.split(run(["pkg-config", "--cflags", "--libs", "OpenEXR"]))
    subprocess.run(["c++", "-std=c++20", source, "-o", sdk, *flags], check=True)
    ours = work / "rypath-probe"
    upstream = work / "pbrt-probe"
    build_probe(args.build, "rypath_exr_output_tests", source, ours, "HDR_IO_RYPATH")
    build_probe(args.pbrt_build, "pbrt", source, upstream, "HDR_IO_PBRT")
    return sdk, ours, upstream


def check_projection(sdk, ours, work):
    run([ours, "film", work])
    matrices = {"demo": ((3.2404542, -1.5371385, -.4985314),
                         (-.9692660, 1.8760108, .0415560),
                         (.0556434, -.2040259, 1.0572252)),
                "pbrt": ((3.240444291419, -1.537133830545, -.498529890988),
                         (-.969266752560, 1.876012242980, .041556048487),
                         (.055643519672, -.204026238798, 1.057226873771))}
    checks = 0
    for model, matrix in matrices.items():
        for precision in ("half", "float"):
            lines = run([sdk, "read", work / f"{model}-{precision}.exr"]).splitlines()
            if lines[0] != f"2 2 {'HALF' if precision == 'half' else 'FLOAT'}":
                raise ValueError("wrong output data window or channel type")
            chroma = list(map(float, lines[1].split()[1:]))
            if any(abs(a-b) > 1e-6 for a, b in zip(chroma, (.64,.33,.3,.6,.15,.06,.3127,.329), strict=True)):
                raise ValueError("wrong sRGB/D65 chromaticities")
            values = list(map(float, lines[2:]))
            xyz = ((1.25 if precision == "half" else 125000,0,0), (0,1,0), (0,0,1), (0,0,0))
            expected = [sum(a*b for a,b in zip(row, pixel)) * (2 if model == 'pbrt' else 1)
                        for pixel in xyz for row in matrix]
            code = 'e' if precision == 'half' else 'f'
            expected = [struct.unpack('<'+code, struct.pack('<'+code, v))[0] for v in expected]
            if len(values) != len(expected) or any(abs(a-b) > 5e-9 * max(1,abs(b)) for a,b in zip(values,expected)):
                raise ValueError(f"independent film projection mismatch: {model}/{precision}")
            checks += len(values)
    print(f"PASS independent installed OpenEXR: {checks} film channels, HALF/FLOAT, demo/PBRT, signed/HDR, chromaticities", flush=True)


def read_levels(text):
    lines = iter(text.splitlines()); values = []
    while (line := next(lines, None)) is not None:
        w,h = map(int,line.split()); values.extend((w,h))
        values.extend(float(next(lines)) for _ in range(w*h*3))
    return values


def check_images(ours, upstream, fixtures, work):
    count = 0
    for name in ("signed-half.exr", "bright-float.exr", "pattern.hdr"):
        path = fixtures / "assets" / name
        for mode in ("source", "mips"):
            for wrap in ("repeat", "clamp"):
                actual_text = run([ours, mode, wrap, path])
                expected_text = run([upstream, mode, wrap, path])
                (work / f"{name}-{mode}-{wrap}-rypath.txt").write_text(actual_text)
                (work / f"{name}-{mode}-{wrap}-pbrt.txt").write_text(expected_text)
                actual, expected = read_levels(actual_text), read_levels(expected_text)
                tolerance = 0 if mode == "source" else 3e-6
                if len(actual) != len(expected) or any(abs(a-b) > tolerance * max(1,abs(b)) for a,b in zip(actual,expected)):
                    raise ValueError(f"PBRT source/mip mismatch {name}/{mode}/{wrap}")
                count += len(expected)
    print(f"PASS pinned PBRT source/mip oracle: {count} dimensions/channel values; HALF/FLOAT/RGBE, repeat/clamp", flush=True)


def stage_fixtures(args, work):
    fixtures = work / "fixtures"
    shutil.copytree(ROOT / "tests/fixtures/pbrt-render/hdr-io", fixtures)
    source = args.pbrt_build.parent / "source/src/ext/openexr/src/test/OpenEXRTest/comp_zip.exr"
    payload = source.read_bytes()
    if len(payload) != EXTERNAL['bytes'] or hashlib.sha256(payload).hexdigest() != EXTERNAL['sha256']:
        raise ValueError("pinned external OpenEXR corpus asset mismatch")
    (fixtures / EXTERNAL['path']).write_bytes(payload)
    return fixtures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build/debug')
    parser.add_argument('--pbrt-build', type=Path, default=ROOT/'.cache/pbrt-reference/build')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--generate', action='store_true', help='regenerate owned EXR fixtures using installed OpenEXR')
    parser.add_argument('--skip-renders', action='store_true')
    args = parser.parse_args()
    args.build = args.build.resolve(); args.pbrt_build = args.pbrt_build.resolve()
    revision = run(["git", "-C", args.pbrt_build.parent/"source", "rev-parse", "HEAD"]).strip()
    if revision != REVISION:
        raise ValueError(f"expected pinned PBRT {REVISION}, got {revision}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='hdr-io-', dir=args.output_dir.resolve()))
    print(f'Artifacts: {work}', flush=True)
    sdk, ours, upstream = probes(args, work)
    (work/"provenance.json").write_text(json.dumps({
        "pbrt_revision": revision,
        "rypath_revision": run(["git", "-C", ROOT, "rev-parse", "HEAD"]).strip(),
        "rypath_dirty": bool(run(["git", "-C", ROOT, "status", "--porcelain"]).strip()),
        "installed_openexr": run(["pkg-config", "--modversion", "OpenEXR"]).strip(),
        "binaries": {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (sdk, ours, upstream, args.build/"rypath_pbrt_reference", args.pbrt_build/"pbrt")},
    }, indent=2)+"\n")
    if args.generate:
        run([sdk, 'generate', ROOT/'tests/fixtures/pbrt-render/hdr-io/assets'])
        rgbe = ((32,64,128,131),(128,64,32,130),(64,128,32,132),
                (96,32,64,131),(32,96,128,130),(128,128,64,129))
        (ROOT/'tests/fixtures/pbrt-render/hdr-io/assets/pattern.hdr').write_bytes(
            b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 3\n' + bytes(v for pixel in rgbe for v in pixel))
    fixtures = stage_fixtures(args, work)
    check_projection(sdk, ours, work)
    check_images(ours, upstream, fixtures, work)
    passed = True
    if not args.skip_renders:
        for scene in SCENES:
            measurements = comparison.render_replicates(args.build/'rypath_pbrt_reference',
                args.pbrt_build/'pbrt', fixtures/f'{scene}.pbrt', work, comparison.QUADRANTS)
            (work/f"{scene}-measurements.json").write_text(json.dumps(measurements, indent=2)+"\n")
            passed &= comparison.compare_regions(scene, measurements)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
