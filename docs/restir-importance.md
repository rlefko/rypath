# ReSTIR importance sampling repair

Area contract 5 retains contract 4's radiance-like target and source-target MIS,
which replaced the contract-3 bounded target and equal-weight reuse. The former target varied by
at most ten percent, so it intentionally surrendered most of the importance
selection that motivates ReSTIR. Its historical correctness and timing results
in [area-reuse.md](area-reuse.md) do not establish a useful MSE reduction.

This remains our full-replay adaptation of [GRIS](https://graphics.cs.utah.edu/research/projects/gris/sig22_GRIS.pdf),
not the complete [Area ReSTIR](https://graphics.cs.utah.edu/research/projects/area-restir/)
algorithm. Fractional footprint matching, lens reconnection and local path
reconnection remain absent. Physical path evaluation, spectral transport,
independent sampling, denoising and tone mapping retain their separate contracts.

## Target and stored weight

For an unexposed signed XYZ leaf contribution `F`, the positive target is

```text
t(F) = max(FLT_MIN, 1e-6 * target_scale)
       + ((abs(X)/4 + abs(Y)/4) + abs(Z)/4)
```

Quarter-L1 is our proxy, not a target prescribed by the papers. It admits signed
spectral contributions and avoids overflowing the sum of finite XYZ components.
Mantissas and exponents are evaluated separately; neither radiance nor a
contribution weight is clipped. Each selected sample stores its target `t` and
normalizer `Z`; its conceptual contribution weight is `Z/t`. The implementation
forms `Z*F/t` in scaled arithmetic rather than materializing a possibly
overflowing intermediate `Z/t`.

## Source-target MIS

All functions below are over the same finite replay domain. Every shifted leaf
is fully reevaluated at its destination. For fresh sample `u_f`, previous selected
sample `u_h`, current target `t_c`, previous-context target `t_h`, and previous
normalizer `Z_h`, warm temporal scores are

```text
s_f = t_c(u_f)^2 / (t_c(u_f) + 20*t_h(u_f))
s_h = 20*Z_h*t_c(u_h) / (t_c(u_h) + 20*t_h(u_h))
```

The coefficients 1 and 20 are fixed, exogenous policy choices, not effective
sample counts or a claim that twenty independent paths were evaluated. A cold
frame uses its fresh sample alone. Using an age or confidence inferred from a
selected sample as a coefficient would need a separate correctness argument.

The extra reverse replay evaluates the fresh payload in the complete previous
successful context. Parameters, texture camera and camera-relative mapping
buffers are owned with the history bank; reusable frame-slot buffers cannot
stand in for that context. Publication swaps context and reservoir together.
A failed same-epoch attempt retains prior history. An epoch change invalidates
eligibility before any potentially failing context allocation.

For four fixed neighbor slots `j`, spatial scores are

```text
s_j = Z_j*t_c(u_j) / (4*(t_c(u_j) + t_j(u_j)))
s_c = sum_j Z_c*t_c(u_c) / (4*(t_c(u_c) + t_j(u_c)))
Z_out = s_c + sum_j s_j
```

The source-MIS factors form a pointwise partition of unity, assuming properly
weighted inputs and fixed, exogenous source functions and neighbor stencil. Correlated inputs
and clamped duplicate slots retain their scheduled shares. There is **no extra
division by pool size**. Pairwise denominators avoid the old constant-coefficient
`destination_target/source_target` amplification when a source is nearly dark
and its destination bright. This is not a global bound on `Z` or output variance.

Four reciprocal target planes reuse the existing neighbor evaluations: the
opposite-direction cache at a neighbor provides `t_j(u_c)`. All four immutable
temporal-cache passes complete before the center contribution is finalized.
Clamped self-neighbors use the center target directly. The cache costs 16 bytes
per pixel per frame slot, or 31.64 MiB at 1920×1080.

## Mutations and final output

The existing symmetric film/lens/wavelength/tape proposals still run between
temporal and spatial reuse. Normalizer MH correction uses the **actual rounded
dyadic acceptance probability**, not the ideal real-valued ratio. Accepted
normalizers are checked before consuming entropy; equality and rejection
preserve the original normalizer bits. See [the MCMC paper](https://arxiv.org/html/2211.00166)
for weighted mutation theory and [the existing proposal record](mcmc-mutations.md)
for our finite-coordinate adaptation. Pinhole lens moves remain latent no-ops;
tail moves regenerate a whole path. Stronger targets do not make these proposals
equivalent to the paper's reconnection-vertex mutations.

The default Area raw output is `conditional-mean`: sum the weighted contributions
already evaluated during final spatial reuse, `sum_i s_i*F_c(u_i)/t_c(u_i)`.
The `selected` ablation outputs `Z_out*F_c(u_selected)/t_c(u_selected)` instead.
Both retain exactly the same selected payload, normalizer and history. This
Rao–Blackwellization removes the final categorical choice's conditional variance;
it is neither a denoiser nor five independent SPP. It is especially relevant to
spectral color noise that persists when only one path's spectral direction is
selected.

Proper weighting and the Rao–Blackwell variance reduction hold in real
arithmetic. Scores, normalizer corrections, contributions and signed sums incur
floating-point rounding. Exact floating-point or success-conditioned unbiasedness
is not claimed. Contract 5 supports subnormal signed contributions and sums;
integer significand rounding implements gradual underflow for these outputs,
including ties to even and values that round to signed zero. Signed addition
scales finite operands into the normal range before arithmetic. This avoids
Metal's permitted flush-to-zero behavior for subnormal operands/results
([MSL specification, §8.1 and §8.5](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)).
Products and quotients still round their intermediate mantissas; the final
conversion rounds that represented value, not the infinitely precise formula.
Positive scores, targets and normalizers retain the normal-only domain.
Overflow and other values outside these domains invalidate the attempt.
The renderer does not drop a candidate, clamp it, or silently retry.

## Accounting and validation

The schedule evaluates `(5+n)*pixels` complete paths after a reset and
`(7+n)*pixels` with history, for `n` mutations. One fresh path per pixel remains
distinct from replay work. Benchmark, qualification, diagnostics and viewer EXR
provenance record contract 5, `quarter-l1-v1`, `source-target-mis-v1`, temporal
coefficient 20 and the output mode. Independent output records these as
inapplicable. Benchmark and diagnostics accept `--area-output selected` or
`--area-output conditional-mean`; the qualification driver applies it only to
its Area arms. The viewer retains independent sampling as its default.

Portable checks cover scaled arithmetic, independent scalar formulas, random
normalizers over multiple reuse/mutation generations, exact rational means and
second moments, and opposite-importance rare tails. A standalone compute-only
Metal probe checks actual GPU dyadic thresholds, extreme support and failures
before entropy consumption. Integration oracles cover analytic cold spatial
stencils, clamped duplicates, unchanged history under output marginalization,
changing-camera temporal denominators, slot reuse and failed publications.

The native Pavilion-day failure was reproduced with a first-failure record:
sample 0, pixel 1,145,311, first spatial neighbor. Its weighted XYZ components
are approximately `(6.4771e-39, 7.0811e-39, 4.1524e-39)`, representable binary32
subnormals rejected by contract 4's output materialization. Contract 5 preserves
them instead of relying on float arithmetic that may flush them to zero.
The same native 1920×1080 seed-51001 cold frame completes with Metal API
validation: 12,441,600 evaluations, 2,073,600 mutation attempts, finite FLOAT RGB.
This is a regression recovery, not a replacement result in the failed study.
Inputs, command, failure and recovery remain in separate local artifact roots
`reservoir-failure-diagnostic-001` and `reservoir-range-recovery-001`.

Release builds and nine focused tests pass: both benchmark/qualification CLI
checks, Area frame/math, mutation frame/weights, reuse-MIS scalar/exact oracles
and the Metal reuse-MIS probe. New cases cover the actual failed operands,
subnormal rounding boundaries and signs, 625 addition edge pairs, 8,192 random
pairs and 8,192 exact cancellations against host IEEE addition. These checks do
not establish scene-wide convergence or performance.

## Completed raw-MSE screen

The measurements below used contract 4, before the gradual-underflow repair;
they are not validation results for contract 5.

The fixed `restir-mse-screen-003` study completed all **212 jobs** at 160×90:
seven estimator/output arms × six seeds × four scenes, sixteen reference jobs,
four matched legacy/current independent controls, and twenty-four independent
8-estimate means. Each reference job averages 1,024 independent estimates; each
scene uses four independently seeded reference realizations. Candidate captures
are the final raw frame after four warmup and twelve measured submissions,
not an average of sixteen SPP. No reconstruction or display transform enters MSE.

The repaired source is `db8d572` (body `d6a6421a…`); the contract-3 comparator is
`b359c92` (body `c11bf70c…`), which preserves the old estimator and adds explicit
large-scene admission to the benchmark. Neither binary is relabeled as unchanged
source `40252cb5…`. The matched independent controls agree in exact final RGB
bits at seed 52001 in all four scenes; this is a bounded observation, not a
universal identity proof.

The table reports **ratios of six-seed mean observed signed-linear RGB MSE**.
`C1` means contract-4 conditional output with one mutation; `I1` is one final
independent estimate, `v3-A1` is contract-3 Area with one mutation, and `I8` is
an average of eight independent estimates. Ratios are descriptive, not ratio
confidence intervals.

| Scene | C1 mean MSE | C1 / I1 | C1 / v3-A1 | C1 / I8 |
| --- | ---: | ---: | ---: | ---: |
| Cornell | 0.64897 | 0.215 | 0.261 | 1.821 |
| Prism | 0.05294 | 0.091 | 0.152 | 0.952 |
| Pavilion day | 1.39571 | 132.249 | 178.531 | 0.138 |
| Pavilion night | 0.09154 | 0.168 | 0.232 | 0.437 |

The predeclared twofold contrast is `candidate MSE - 0.5*control MSE`, paired by
seed with a nominal pointwise 95% Student-t interval (six seed clusters, df=5). Only Cornell
versus I1 and prism versus I1/v3-A1 have strictly negative upper bounds, also
under all four leave-one-reference-out checks. Other primary contrasts remain
unresolved. These intervals condition on the finite reference; reference batch
variance and sensitivity checks do not establish global convergence or
simultaneous coverage. All twelve mutation-one minus mutation-zero intervals
(v3, selected v4 and conditional v4 across four scenes) include zero. This screen
does not establish an incremental MCMC benefit.

Pavilion day's aggregates retain severe finite bright-pixel tails. At seed
52004, one pixel accounts for over 99.92% of squared error in all four v4
output/mutation arms. A separate independent-I8 seed 52006 event contributes
99.99% of that image's error; its final raw frame is a separate, ordinary
capture and cannot substitute for the eight-input mean. No outlier is excluded
or replaced. Thus the favorable day C1/I8 ratio is not a robust improvement
claim. Posthoc localization does not identify a causal path or distinguish
rare transport from reuse/target amplification without retained path tapes.

Work is unequal: C1 evaluates eight complete paths per warm frame and 126 per
pixel across its sixteen submissions, including history setup. I8 matches only
the warm-frame evaluation count; it excludes those history and setup costs and
is not an equal-time or total-work comparator. Cornell C1 is 82% worse than I8
by mean MSE, while prism is about 5% lower. The repaired estimator is not
qualified as generally more efficient, and independent sampling remains the
viewer default.

The separate native follow-up stopped after ten of sixteen jobs completed:
four independent controls and three conditional-Area1 seeds each for Cornell
and prism. The first Pavilion-day conditional-Area1 job (seed 51001) failed
`GPU Area reservoir validation failed` on sample 0, with zero completed frames
and no history use. Its failure report is retained; the raw output is only a
reservation, not an image. Five jobs remain unattempted. No retry, resume or
full-matrix reduction was performed, and partial native results do not qualify
a setting. Native quality and equal-time/total-work acceptance remain open.
The retained `native001-pavilion-day-failure-inventory-001.json` verifies the
failure files and binding before/after (SHA-256
`913d7782fa9bd7302a9b00e2d146baa629acb500ca6d11e7addbe17cf3eaff0d`).

The retained report is `.cache/validation/restir-mse-screen-003/results-001.json`
(SHA-256 `5ab53b2329ea9986b234252201f65e197d78d960c0a5bfc133acee670d91fb55`).
A separate CPU execution audit reran the frozen reducer, reproduced the report
exactly, and verified all 212 completion inventories before and after reduction
(`screen003-results-verification-001.json`, SHA-256
`6e1bb870805ab653434e2eaf2f53097144e705023cc736a444272bb15c2bacac`).
The separate `pavilion-tail-audit-001.json` records finite-pixel localization
(SHA-256 `79b92118ef70b4dd9b9d091e105e522d0cbda8607b5b856e84e1f2ab29ce3b72`).
These ignored local artifacts preserve numerical/provenance evidence; they are
not additional public corpus assets.

## Reconciled MCMC-stage validation

The forward-only MCMC-stage integration at `df65c62` passes the supported Debug
configure/build and all 247 mandatory tests on the native host, with no skipped
tests. This includes the Area/MCMC integration checks and scalar, exact-rational
and Metal MIS probes. Subsequent reconstruction/profiling/guide/MetalFX merges
have source checks and normal commit-hook validation; their combined native
validation is pending. The upstream screening and recovery evidence above remains
bound to its original revisions and does not qualify this consolidated tree's
image quality or performance.
