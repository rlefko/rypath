# PBRT sampling and default descriptors

This change adds Halton, ZSobol, normalized tabulated Gaussian reconstruction,
and pinned PBRT defaults for omitted camera, film, filter, sampler and integrator
records. Samplers change sample placement/correlation; Gaussian changes the
filtered image integral. Transport PDFs, compensated roulette, raw XYZ sums and
counts remain independent of presentation. Tests provide bounded evidence, not
proof of general PBRT parity or unbiasedness.

## Contract and provenance

Semantics follow PBRT `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`.
The [render profile](pbrt-rendering.md#halton-and-zsobol-sampling) fixes sampler
modes, dimensions, seed behavior, effective ZSobol sample count and range limits.
The [source record](../third_party/pbrt/sampling.md) identifies adapted routines
and the checksum-pinned prime table. Production has no PBRT runtime dependency.

Omitted descriptors resolve without changing authored records. Camera identity,
vacuum and absence of a named camera coordinate system remain independent of
option graphics state; explicit Camera captures its own state. Defaults and
ZSobol authored/effective counts appear in output provenance.

Gaussian compilation retains pinned binary32 midpoint, FastExp and CDF operation
order, with contraction disabled. Its positive table defines the reconstruction
model. PBRT's tabulated `f[cell]/pdf` is mathematically constant, so normalized
unit weights retain count-based accumulation. Zero rows/cells are skipped by
CDF inversion; invalid or collapsed distributions fail rather than substituting
a uniform distribution. Normalization values and positive CDF gaps must survive
Metal subnormal flushing. Storage is bounded before allocation and owned by the
native session. Primary rays and texture differentials share sampled offsets.

Identity/renderer/sampling contracts advance to 10/12/3; format and accumulation
remain 1. Existing independent/Sobol arithmetic is preserved. Older checkpoint
contracts are rejected; fixed effective sample budgets remain part of identity.

## Acceptance criteria

- All 32 explicit/implicit descriptor combinations produce the same canonical
  scene; state capture, unsupported authored descriptors, source locations,
  sampler limits and Gaussian failure cases receive dedicated portable checks.
- Halton/ZSobol known answers come from the separately linked pinned CPU sampler.
  Integer indices and ZSobol words compare exactly. Native Halton variates use
  a bound of eight binary32 epsilons relative to their nonzero magnitude.
  High absolute indices, all modes, scalar/pair hashes and extension tails are
  exercised. No confidence intervals are inferred from unchanged Halton
  `none`/`owen` streams.
- Gaussian CDFs and production sample pairs compare with a linked PBRT oracle.
  Independent midpoint quadrature checks the distribution. A radius .0625,
  sigma .01 two-cell filter has edge probability .25; a continuous Gaussian
  would give approximately .000889. Native tests check this distinction,
  constant-sky unit weights, beyond-film support, lens focus, stored ownership,
  invalid tables and per-path geometric predictions.
- Reordered requests, batch partitions and checkpoint/resume retain sample
  identity. ZSobol rejects missing/changed target budgets and requests outside
  the fixed count, avoiding Morton-index aliasing.
- Eighteen image cases use 8x8 pixels, 4,096 spp and 16 fixed seed replicates per
  renderer. Each full-frame/quadrant RGB mean must agree within five combined
  standard errors plus `2e-5 * max(1, abs(reference))`: 270 fixed gates.
  Halton permuted digits and three randomized ZSobol modes use replicate
  statistics. A separate 16-spp pair checks exact implicit/explicit defaults
  within each renderer. Seeds and thresholds are fixed before measurement.

The [fixture manifest](../tests/fixtures/pbrt-render/sampling-defaults/README.md)
records owned edge, diffuse, lens/texture and volume fixtures plus attributed CC0
Cornell derivatives. Relocated fixtures retain checked dependency closures.
These establish selected derivative coverage, not support for additional
unmodified upstream entry points or the full scene collection.

```sh
python3 scripts/compare_sampling_defaults.py \
  --rypath build/release/rypath_pbrt_reference \
  --output-dir .cache/sampling-defaults/reference
python3 scripts/generate_sampling_default_samples.py \
  --output-dir .cache/sampling-defaults/previews --render
```

## Measured evidence

Measured on 2026-11-16 for implementation `ba1982bb6bfdb7130230e2ade7f3cb5d19de715a`:
Apple M4 Max (40 GPU cores, 128 GB unified memory), macOS 26.6 (`25G5065a`),
Xcode 26.6 (`17F113`), Apple Clang 21.0.0, CMake 4.0.3 and Ninja 1.13.2.
The separate portable build used LLVM 18 with its matching libc++ headers.

- Debug and ASan/UBSan CTest: 186/186 each passed. Release also passed all
  186 checks, including a focused rerun after rebuilding the CLI availability
  test fixture.
- Full M4 Max API validation: 48/48 Metal tests passed with
  `MTL_DEBUG_LAYER=1`, with no availability skips.
- Portable CTest: 140/140 passed; Python harness/policy suite: 186/186 passed.
  Pre-commit, focused LLVM 18 clang-tidy and the Apple analyzer passes on the
  changed Objective-C++ adapters passed.
- The 18-case pinned CPU PBRT comparison passed all 270 declared gates.
  The largest difference/threshold ratio was 0.641087: diffuse-area ZSobol Owen,
  top-left green, absolute difference 0.00320964 against threshold 0.00500656.
  Both exact within-backend implicit/explicit default pairs passed with zero
  pixel difference and positive signal. This does not assert equality between
  the two renderers at 16 spp.
- All 81 legacy raw PFM outputs and the demo's raw checkpoint pixel payload
  matched the retained `b2eae1c` main artifacts byte for byte. Inputs included
  prior lighting/material/media/importer/filter fixtures plus area-power,
  box-filter and mesh-shape cases. Contract headers intentionally changed.
- All 28 independently generated PBRT sampler known answers passed. The native
  suite compares 980 CPU/MSL queries, covers the 52-bit and signed-64-bit index
  boundaries, and executes roulette, emitter selection and glass-slab paths.
  Checkpoint tests cover all seven new sampler modes, changed batch sizes and
  rejection of incompatible budgets, seeds and filters.

Native validation commands:

```sh
MTL_DEBUG_LAYER=1 ctest --preset release --no-tests=error --verbose -R '^metal\.'
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ./build/release/rypath_qmc_render_tests
```

The targeted QMC test also passed with both `MTL_DEBUG_LAYER=1` and
`MTL_SHADER_VALIDATION=1`. An additional full-suite shader-validation attempt
hit the existing 60-second STL test timeout and was stopped. The same STL
executable subsequently passed with both flags when run directly without that
CTest time limit. Full-suite shader validation is therefore not claimed.

The independent Gaussian oracle is the optional
`RYPATH_PBRT_GAUSSIAN_ORACLE` branch of `tests/gaussian_filter_tests.cpp`, linked
against the pinned Float PBRT library with `-ffp-contract=off`. Its FNV-1a hashes
cover explicit little-endian binary32 bytes. All CDF entries and a 33x33 sample
grid matched exactly after the final numerical changes:

| Radii; sigma | CDF entries | CDF hash | 1,089 sample-pair hash |
| --- | ---: | --- | --- |
| 1.5, 1.5; .5 | 2,401 | `c3f9ee5098fe6b9a` | `ee5857e52f638c40` |
| 1.5, .75; .5 | 1,201 | `1e3b50fa53faba43` | `e21428c7748daa49` |
| .1, .16; .04 | 26 | `64d7a87137843fa9` | `fadf77a69a345d53` |

Reference-run artifact SHA-256 values:

| Artifact | SHA-256 |
| --- | --- |
| Pinned PBRT CPU executable | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |
| rypath reference executable | `e92650ec4da90200d4b367fde75aa95c8fa7877313b83e6e739d7bf847515c6c` |
| rypath Metal library | `9692a60be628e9c67ad3ac5afecf3b96a58b0152eba237bde72590533ba11b5c` |

Local logs, seed measurements, source/binary hashes and default-pair outputs are
retained under `.cache/sampling-defaults/`, including
`reference-final/sampling-defaults-msg5lw01/` and `final/report.json`.
The comparison provenance records the clean implementation commit above.
Preview scenes and renderer-produced display PNGs are under `previews/`; labels,
commands and hashes are in its gallery and JSON record. No denoising was used.
These are correctness/compatibility checks, not a performance study, a claim of
matching PBRT's downstream path sequence, or full corpus acceptance.
