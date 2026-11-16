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

Implementation validation is in progress. The final record will identify the
exact implementation revision, toolchain, device and completed checks. Earlier
records describe their own revisions and are not current test results.
