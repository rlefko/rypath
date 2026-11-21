# Reproducing the Layered Schedule Oracles

`pbrt_layered_schedule_probe.cpp` calls the unmodified PBRT v4
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f` `f`, `PDF`, and `Sample_f` routines.
It reads only the input queries in `tests/layered_schedule_oracles.h`; expected
results never enter the oracle. This is a standalone CPU tool, not part of
rypath or its Metal build.

First prepare the pinned binary32 CPU PBRT build described in
[PBRT render validation](../docs/pbrt-render-validation.md). From the repository
root, the following command reuses that build's exact compiler definitions,
include paths, arithmetic flags, and link dependencies without rebuilding it.
It writes only to `.cache/coating-fidelity/reproducible-oracle/`.

```sh
python3 - <<'PY'
import hashlib, json, shlex, subprocess
from pathlib import Path
root = Path.cwd()
build = root / '.cache/pbrt-reference/build'
source = root / '.cache/pbrt-reference/source'
out = root / '.cache/coating-fidelity/reproducible-oracle'
out.mkdir(parents=True, exist_ok=True)
revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
assert revision == 'b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f'
subprocess.run(['git', '-C', str(source), 'diff', '--quiet', 'HEAD'], check=True)
lines = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'pbrt'], text=True).splitlines()
compile = shlex.split(next(line for line in lines if 'pbrt/cmd/pbrt.cpp.o -c' in line))
compile = compile[:compile.index('-MD')]
assert '-ffp-contract=off' in compile and '-O3' in compile
wrapper = root / 'scripts/pbrt_layered_schedule_probe.cpp'
obj = out / 'probe.o'
exe = out / 'pbrt_layered_schedule_probe'
compile += ['-I' + str(root / 'src'), '-c', str(wrapper), '-o', str(obj)]
link = shlex.split(lines[-1])
link = link[2:link.index('&&', 2)]
link[link.index('CMakeFiles/pbrt_exe.dir/src/pbrt/cmd/pbrt.cpp.o')] = str(obj)
link[link.index('-o') + 1] = str(exe)
for command in (compile, link):
    subprocess.run(command, cwd=build, check=True)
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
files = [wrapper, root / 'tests/layered_schedule_oracles.h',
         root / 'src/layered_probe_wire.h', source / 'src/pbrt/bxdfs.h',
         source / 'src/pbrt/bxdfs.cpp', source / 'src/pbrt/util/sampling.cpp',
         build / 'libpbrt_lib.a', exe]
(out / 'build-receipt.json').write_text(json.dumps({
    'pbrt_revision': revision, 'commands': [compile, link],
    'sha256': {str(path): sha(path) for path in files}}, indent=2) + '\n')
PY

.cache/coating-fidelity/reproducible-oracle/pbrt_layered_schedule_probe --fixtures \
  > .cache/coating-fidelity/reproducible-oracle/results.txt
.cache/coating-fidelity/reproducible-oracle/pbrt_layered_schedule_probe --queries \
  > .cache/coating-fidelity/reproducible-oracle/queries.txt
.cache/coating-fidelity/reproducible-oracle/pbrt_layered_schedule_probe \
  < .cache/coating-fidelity/reproducible-oracle/queries.txt \
  > .cache/coating-fidelity/reproducible-oracle/roundtrip.txt
cmp .cache/coating-fidelity/reproducible-oracle/results.txt \
    .cache/coating-fidelity/reproducible-oracle/roundtrip.txt
```

There is one output row per fixture, in header order, with nine significant
decimal digits preserving binary32 values. Each row contains:

```text
f[0] directional_PDF joint_weight joint_PDF wi.x wi.y wi.z pdfIsProportional
```

`joint_weight` is `Sample_f.f[0] * abs(wi.z) / Sample_f.pdf`. Missing or zero-density
samples have zero sample fields. The directional PDF and joint-path PDF are
separate quantities; rypath's sampled directional MIS proxy must not be compared
to the upstream joint-path PDF.

The versioned stdin format exported by `--queries` is:

```text
RYPATH_LAYERED_SCHEDULE_1
query_count
eta ax ay base_eta base_k base_ax base_ay reflectance thickness albedo g ox oy oz ix iy iz uc u v conductor_base max_depth samples event_seed operation material_seed
```

The field-name row above describes each numeric record; it is not part of the
input. Operation `0` selects coated radiance, `3` coated importance, and `4` the
unmodified `DiffuseBxDF` base-map routine. The conductor flag selects the coated
base for operations `0` and `3`. `event_seed` is a validated unsigned32 field
retained for the GPU query ABI; PBRT ignores it. Signed32 `material_seed` is set
as PBRT's global `Options.seed` before each query. Raw local float directions
are passed unchanged, without normalization or frame reconstruction.

The wrapper rejects malformed/trailing records, unsupported operations,
nonfinite or invalid material/random inputs, nonunit/tangent query directions,
out-of-range integer fields, zero internal samples, depth/sample counts above
1024, and query counts outside 1..65536. Invalid input exits with status 2 and
no result rows.

These fixtures check selected domains, draw schedules, rejection branches,
roulette and concentric mapping. They do not promise universal per-seed or
bitwise CPU/Metal BSDF equality. In particular, the nonaxial HG frame construction
is a distribution-equivalent implementation difference. Existing numerical
bounds account for CPU/Metal arithmetic; base-map corners compare `z²` because
one sin/cos ULP can round grazing `z` to zero. Statistical route-moment and image
gates remain separate checks.
