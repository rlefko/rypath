# Conductor validation

The conductor slice targets PBRT v4
[`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f):
smooth and rough anisotropic GGX reflection using complex eta/k spectra.
Roughness follows PBRT's square-root remap when enabled. This is a
single-scattering microfacet model; a multiple-scattering white-furnace identity
is not an acceptance criterion. Optical eta/k must remain independent from
clamped diffuse reflectance. RGB/blackbody optical constants, optical texture
graphs, and PBRT's reflectance shortcut are outside this slice.

The [fixture manifest](../tests/fixtures/pbrt-render/conductor/README.md) records
eight small owned wrappers and selected external PLY coverage. It does not
establish support for the original Sportscar scene or the full scene corpus.
The pinned PBRT reference remains an external CPU build; production rendering
uses native Metal acceleration and raw spectral accumulation.

The native `conductor_render_tests.cpp` checks constant eta=1.5, k=2 at normal
incidence against the independent complex-index Fresnel identity
`((eta-1)^2+k^2)/((eta+1)^2+k^2)`. Environment reflection and a finite emitter
behind the camera check delta throughput and emissive-hit unity MIS. A constant
image environment is compared with its unobstructed value times
Fresnel to check unity delta MIS. An invisible finite emitter contributes to
rough-conductor NEE and contributes zero through smooth BSDF hits. Mixed
mirror/diffuse/emitter and diffuse/mirror/emitter scenes require positive
two-scattering emission and zero emission when cut off at depth one. The latter
uses a one-sided emitter dark from the primary diffuse surface. All eight
fixture configurations check exact sample identity under 113/144 sample batch
partitioning and threadgroup widths 1/32/64, using independent and Fast Owen
Sobol sampling. These invariants compare wavelength, radiance, scatter count,
absolute sample index and raw camera random words. Production progressive
rendering additionally requires exact raw compensated Film equality across batch
sizes 1, 7 and 33 for smooth and roughness-graph scenes with both samplers. A
negative dynamic checker roughness branch must fail the staged batch with zero
committed samples. A full 8×8 graph sensitivity check requires exact matches to
both fixed-zero
and fixed-0.15 roughness response plateaus. The full-frame finite roughness
response is checked separately against upstream rendering.

```sh
python3 scripts/compare_conductors.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --output-dir .cache/pbrt-reference/conductor-validation
```

The roughness-graph comparison changes its zero checker endpoint to 0.01 for
both renderers, retaining the other endpoint at 0.15. This declared override
avoids the pinned checker's negative cancellation tail before square-root
remapping. It does not change the native zero/0.15 regression. The harness
records the exact override and original/comparison source hashes; the other
seven fixtures are unchanged.

Before observing comparison results, gates are fixed at 8×8 pixels, 4,096 spp,
16 independent fixed seeds per backend (rypath 0–15; PBRT 16–31), full image and
four quadrant means in each linear RGB channel. Each mean must agree within
five combined standard errors plus `2e-5 * max(1, abs(reference))`. No display
encoding, tone map or denoiser participates. Existing strict PFM readback
preserves signed and HDR values and verifies dimensions and payload. The
harness records binary hashes, Git revisions, dirty state, retained PFM files
and per-seed measurements. A failed gate is a failure requiring investigation;
seed selection and reruns chosen to pass are excluded.

## Scattering and numerical checks

The reflection model uses the visible GGX normal distribution. Rough events use
reflected-direction density `D(m) / (4 abs(wo.z) (1 + lambda_o))` and throughput
weight `F (1 + lambda_o) / (1 + lambda_o + lambda_i)`, algebraically
`f abs(wi.z) / pdf`. The PDF retains its null-event probability mass. Smooth
reflection has unit delta mass and Fresnel weight; it receives no continuous
light proposal and full weight for subsequent emitter/environment hits. Rough
area/environment proposals and BSDF-hit emission use the power heuristic with
their matching densities. Invisible emitters retain their existing light-only
proposal. The existing wavelength, sampler dimensions and compensated 0.75
roulette after three scatterings are unchanged.

Tangent outgoing directions, below-hemisphere reflection, PBRT's explicit
`m.z^4 < 1e-16` distribution cutoff, and zero Fresnel are valid null events.
They terminate continuation while retaining already accumulated contributions.
Invalid arithmetic or negative evaluated roughness rejects the whole staged
Film batch. No contribution clamp or multiple-scattering compensation is added.
Mapped tangents preserve anisotropy through normal/bump maps and mirrored UVs;
only orthogonality error within `64 * FLT_EPSILON` is repaired. Geometric normals
continue to control offsets and emitter sidedness.

The production Metal BSDF is checked against independent double equations:

- 5,493 point/sample cases: 4,096 seeded ordinary cases, 240 roughness/grazing/
  endpoint cases, 77 optical-limit cases and 1,080 crossed optical/critical-angle
  cases. These include both hemispheres, zero axes, smooth/floor boundaries,
  roughness above one, cosines down to `1e-6`, random endpoints, and eta/k from
  `1e-10` through `1e10`.
- 4,096 ordinary cases also compare against the actual pinned PBRT
  `ConductorBxDF`, `FrComplex` and GGX sampler. Cancellation-sensitive limits
  use the independent double oracle instead of PBRT's float result.
- 128 reciprocity and 128 axis-swap comparisons, sampled/evaluated PDF and
  weight consistency, and 24 additional invalid-input/null/overflow contracts.
- 524,288 Monte Carlo samples across four isotropic/anisotropic cases compare
  reflected energy and accepted probability mass against independent hemisphere
  quadrature, refined from `512 x 1024` to `1024 x 2048`. This checks the
  single-scattering energy bound, not a multiple-scattering furnace identity.
- Eight mapped normal/bump frames, including mirrored UVs, compare with double
  projection/displacement equations and orthogonality at `4e-6` absolute.

Fresnel tolerance is `2e-6 + 2e-5 * abs(reference)`; BSDF, PDF and weight use
`2e-6 + 3e-4 * abs(reference)`; sampled direction components use `3e-5` absolute.
Monte Carlo energy and accepted mass use five standard errors plus `2e-4`, with
quadrature refinement difference below `2e-5`. Flags and sample accounting are
exact. These tolerances were fixed before observing results.

Two stable arithmetic adaptations retain the target equations. Complex Fresnel
computes `eta^2 - k^2 + cos^2(theta) - 1` with FMA square residuals and
error-free addition residuals, then forms the dominant complex square-root
component first. This preserves both near-vacuum grazing and small-index normal
limits. Visible-normal sampling retains the outer-disk `1-u` residual and
rationalizes a subtractive term near the disk boundary. Neither adaptation
replaces invalid arithmetic with a plausible contribution.

## Revision evidence

The implementation revision is `6be5a4b`, including the checker prerequisite
`d64d632`, based on merged main `edaeb6da`. The cached PBRT revision is the one
above. Measurements use Apple M4 Max (`applegpu_g16s`), macOS 26.6
(`25G5065a`), Xcode 26.6 (`17F113`) and Apple Clang 21.

The final implementation passes 91/91 tests in each Debug, Release and
ASan/UBSan preset. Metal-disabled Apple Clang and Clang 18 builds each pass
70/70. All 23 Metal tests pass API validation, including the new conductor
transport and BSDF tests. Changed-unit Clang-Tidy, Apple host static analysis,
77 Python checks and repository hooks pass. Logs are retained under
`build/conductor-final-*`, `build/conductor-metal-api.log`,
`build/conductor-tidy*.log` and `build/conductor-native-analysis.log`.
The final Release image comparison passes all 120 gates across 256 PFMs, with
all 256 process records free of error diagnostics. Artifacts are retained in
`.cache/conductor-reference-final/conductors-76zk6m6c` and the command log in
`.cache/conductor-reference-final.log`. Provenance identifies implementation
`6be5a4b`, the uncommitted harness/documentation state and Release executable
SHA-256 `2ac4a8f838dd3de52c6c3258ec7fe11ff8e922467e52ea709f729d5fe1cb575a`.
No production source changed after these measurements.

The whole shader-instrumented CTest run passed 20/23: texture footprint reached
its 30-second limit, while combined-texture and conductor transport reached
180 seconds. Apple's compiler service was active during the long waits.
Direct instrumented reruns with a 600-second diagnostic cap pass all three
without shader validation errors, completing instrumented coverage of all 23
tests. The final standalone BSDF probe also passes all 4,096 pinned PBRT
comparisons (`build/conductor-final-pinned-bsdf.log`). These longer diagnostic
runs do not change normal CTest limits or statistical thresholds. Logs are
`build/conductor-metal-shader.log` and `build/conductor-shader-extended-*.log`.

The earlier dirty-tree comparison in
`.cache/conductor-reference/conductors-jstz9a_w` is **not accepted reference
agreement evidence**: PBRT emitted 682 NaN-radiance errors and replaced affected
samples with black while exiting successfully. Finite PFM readback could not
detect that substitution. The final harness retains each renderer's stdout and
stderr and rejects error diagnostics even after a successful exit. The original
zero/0.15 native roughness fixture and its failure/Film regressions remain intact.
Individual replays localize the error to that fixture: seed 16 alone reports 39
substitutions; the other seven fixtures are clean. All 128 PBRT runs with the
positive-floor comparison variant pass strict diagnostics and finite readback
in `.cache/conductor-reference-diagnostics/pbrt-positive-floor-zprheqls`. These
CPU-only checks establish clean reference inputs, not image agreement.

## Filtered checker numerical adaptation

The original cancellation-prone checker antiderivative difference could turn a
valid `[0, 0.15]` roughness pair negative. For example, coordinate `3.04970002`
and radius `0.0500000007` yielded axis value `-1.00002277`; combining two such
axes yielded blend weight `-2.27692344e-5` and negative roughness. The corrected
fixture exposed a GPU arithmetic failure instead of silently accepting this
invalid value. The original zero/0.15 fixture remains a native regression; reference runs
with upstream error substitution are excluded from agreement claims.

The checker now integrates the tent's triangular caps directly for radii at
most one. Broader footprints use the periodic remainder of the square wave's
second antiderivative after cancelling the linear term. Coordinates and radius
are reduced by the period before adding, and division by radius occurs twice
to avoid squaring overflow. The nearest upper boundary is subtracted directly
near negative zero to preserve small distances. These are equivalent integral
forms; no roughness, graph output or radiance clamp is introduced.

An independent double reference splits the tent at each unit-cell boundary and
its apex, then integrates each linear segment. A standalone CPU run passed all
279 adversarial cases with maximum axis error `5.94e-8`. The cases cover the
observed failures, signed boundaries, zero/tiny/broad footprints and large
finite coordinates. Even-integer radii spanning whole periods additionally use
the exact zero-average identity. The integrated CPU and MSL graph probes pass
these cases with an `8e-7` oracle tolerance and require bounded graph values.

The original pinned source-float graph comparison was run without changing its
thresholds: 6,436 of 6,440 values pass; four checker values differ by more than
`3e-5`. The stable results agree with the independent double piecewise integral
to less than `4e-8`. These four source-float disagreements remain explicit;
they are not reported as 6,440 passing comparisons.

| Query / graph | Double integral | Stable rypath | Pinned PBRT | PBRT absolute error |
| --- | --- | --- | --- | --- |
| 38 / spectrum checker | 0.811075709907 | 0.811075747 | 0.811116993 | 4.13e-5 |
| 58 / scalar checker | 0.362976722101 | 0.362976730 | 0.362892926 | 8.38e-5 |
| 74 / scalar checker | 0.281838691837 | 0.281838685 | 0.281899244 | 6.06e-5 |
| 89 / spectrum checker | 0.274454666121 | 0.274454653 | 0.274496496 | 4.18e-5 |

The four exact contexts, transforms and graph operands are retained in
`tests/checker_reference.hpp`. New CPU/MSL controls require agreement with the
independent integral under the original `3e-5` threshold and retain PBRT's
baseline error. The original comparison script and its tolerances are unchanged.
Reference artifacts are in `.cache/conductor-checker-reference/results`, with
log `.cache/conductor-checker-reference.log`. This explicitly targets the
filtered checker integral over pinned binary32 cancellation artifacts.

Earlier diffuse/HDR test counts are historical evidence for those completed
slices, not validation of this conductor change.

## Reproducing the checks

Use the supported configure/build/test presets and the strict Metal API command
in [Metal execution](metal-execution.md). Additional shader instrumentation is
`MTL_SHADER_VALIDATION=1 ctest --preset debug --no-tests=error --verbose -R '^metal\.'`.
The standalone BSDF probe accepts an optional executable built against the
pinned external CPU reference. This reuses that configured Ninja build's actual
compiler and linker flags without changing its build graph:

```sh
python3 - <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, 'scripts')
from compare_hdr_io import build_probe
root = Path.cwd()
build_probe(root/'.cache/pbrt-reference/build', 'pbrt',
            root/'scripts/pbrt_conductor_probe.cpp',
            root/'.cache/pbrt-reference/build/rypath_conductor_probe',
            'RYPATH_CONDUCTOR_REFERENCE')
PY
build/debug/rypath_conductor_probe \
  --pbrt-probe "$PWD/.cache/pbrt-reference/build/rypath_conductor_probe"
```

No external renderer is required for routine CTest. The image comparison command
above is opt-in and retains all seed images and process transcripts. The
[performance record](conductor-performance.md) separately measures legacy
workload cost; statistical agreement and throughput are different claims.
