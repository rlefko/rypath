# PBRT sampling and Pavilion convergence

The [transport/intersection regression record](transport-regression-validation.md)
diagnoses the raw night outlier and day repeatability difference retained below.
Its renderer-18 candidate completes fresh day and night images, including exact
checkpoint-to-FLOAT and checkpoint-to-PNG audits. The completed study fails all
48 simultaneous quality bounds and 10 of 36 raw RGB mean safeguards. Repeated
timing also measures a slowdown. Diagnosis continues; earlier failed studies
remain unchanged historical evidence.

This follow-up changes light proposals and the stochastic coated-material
approximation. It preserves separate raw spectral accumulation and exposed,
clamped Film planes. It does not add denoising, outlier rejection, or a new
physical coating model. The baseline is the Pavilion implementation merged in
[PR #50](https://github.com/rlefko/rypath/pull/50), `dd8682d5`.

## Sampling contracts

All supported scalar surface, homogeneous-medium, packet path and BDPT routes
use stable light identities. Area primitives precede positional sources, distant
sources and an actual environment. A synthetic black environment is omitted.
Path/volpath accept `bvh`, `power`, or `uniform`; omitted policy selects BVH.
BDPT always uses power selection. An authored `lightsampler` parameter on BDPT
is diagnosed because the pinned integrator has a fixed policy.

Power selection uses an alias table. The light BVH uses PBRT's angular bounds
and 12-bucket SAH construction, with deterministic partitioning, conservative
bounds and a bounded-depth construction fallback. Selected-light probabilities
are part of both forward samples and reverse MIS queries. Delta-position and
delta-direction incident contributions have unit MIS weight and reciprocal
selection probability. A selected black or unsupported-direction proposal
contributes zero without discarding the path.

Environment sampling retains native texel resolution. Double-precision host
prefix sums produce binary32 GPU CDFs; queried density uses the actual stored
intervals. The full proposal mixes 15/16 image density with 1/16 uniform sphere
density to retain support when a positive interval collapses during rounding.
Unidirectional NEE uses a separate compensated table with
`max(texelMean - globalMean, 0)` weights. Its omitted directions are covered by
BSDF/phase continuation. BDPT uses the full table in every strategy. Black or
constant compensated image tables fall back to uniform sampling. Constant
non-image environments use continuation alone in unidirectional transport,
matching PBRT's incomplete-PDF policy. Retained-image and preparation budgets
include the full and compensated native tables.

The inherited packet camera-path estimator still uses power MIS without outer
roulette for both `path` and vacuum `volpath`. Pinned PBRT uses power MIS for
`path`, balance MIS for vacuum `volpath`, and compensated outer roulette for
both. For a consistent exact BSDF, both approaches preserve the raw finite-depth
expectation, but can change moments and means after nonlinear Film clamping.
The finite coating approximation remains a separate limitation. These differences
are not a demonstrated explanation for a particular Pavilion outlier. Both BDPT
implementations omit outer roulette. The frozen quality gates remain unchanged.

Incident geometry and emitted-ray measures remain separate:

- Triangles use spherical sampling with the receiver cosine warp between the
  declared solid-angle thresholds, otherwise an area proposal.
- Rectangular patches use spherical sampling above `1e-4` steradians. General
  patches use corner-Jacobian bilinear importance and the actual local Jacobian.
- Full similarity-transformed spheres use an external cone proposal. Clipped,
  nonuniform/sheared, inside and uncertain-boundary cases retain area/Jacobian
  sampling. Disk and open-cylinder area Jacobians remain active.
- BDPT emission uses the matching area proposal and cosine direction density.
  Point lights emit uniformly; spot cones use their cubic falloff; goniometric
  maps use native equal-area CDFs; projection maps include the perspective
  `cos³` factor. Normalized affine direction transforms carry their Jacobian.
  Mapped emitted rays retain a `1/65536` uniform UV mixture for positive support.

The triangle implementation rationalizes a small-angle half-angle expression
and uses spherical-subtriangle inversion instead of PBRT's angular cutoff.
Rectangle sampling removes an internal uniform-point shortcut above our declared
threshold. These are measured numerical adaptations, not bitwise PBRT sampling
claims. Alpha and visibility retain the existing endpoint and blocker rules.

## Coated-material compatibility

The target is the finite approximation in pinned PBRT, with authored internal
`maxdepth` and `nsamples`, not equality between its sampled joint walk and its
separately estimated directional value. Value, PDF and walk routines use PBRT's
typed float hashes and separate PCG seed domains. A signed 32-bit material seed,
default zero, is independent of sampler seed and exposed as `--material-seed`.
BDPT retains exact ordered local directions for previously sampled edges; new
connections use their own directions. No angular quantization joins distinct
PDF queries.

The coating routines retain PBRT's otherwise unused interface-choice draws,
entrance-rejection short circuit, roulette interval and binary32 survival
arithmetic. Diffuse bases use its concentric cosine map; tangent phase proposals
leave the value estimator's path state unchanged. Fixed raw-local-input fixtures
check these routes directly against unmodified PBRT. CPU/Metal arithmetic can
still change grazing-corner acceptance, and the nonaxial HG frame maps a given
random pair differently while preserving the ideal azimuthal distribution.
These checks do not promise universal per-seed BSDF or pixel identity.

Diagnostic builds retain the planned PDF fixed/hash × value event/hash matrix.
The production configuration uses the pinned hash domains for both. The original
45 coated image gates remain the acceptance criteria. Independent moment checks
compare each estimator route against the corresponding PBRT route; they do not
force the finite joint-walk and directional estimators to agree with one another.

## Completed renderer-18 scene study

Frozen candidate v8 (`c3489bf`, contracts 3/12/18/7/2) completes the unchanged
320×170 study: day at 256 spp and night at 512 spp, 32 replicas per backend and
scene. All 64 fresh candidate records and the original 128 PBRT/baseline
references are authenticated, including all 384 raw/clamped FLOAT EXRs. Candidate
seeds are 0–31, PBRT 32–63 and merged-baseline 64–95; no seed is replaced. The
same six regions, 48 variance/tail endpoints, 36 raw RGB mean safeguards,
20,000 bootstrap draws and 1.25 acceptance limit remain fixed.

**The completed study fails: 0/48 simultaneous quality bounds and 26/36 raw mean
safeguards pass.** Day passes all 18 means; night passes 8/18. Fifteen of the 48
point ratios exceed 1.25. The remaining point ratios do not establish acceptance:
all simultaneous upper limits exceed the target.

The unchanged empirical, percentile-style joint bootstrap adds the nonnegative
95th percentile of the maximum centered bootstrap log-score deviation across
all 48 endpoints to each point log score. Its log margin is 3.298993620567619
(a multiplier of 27.0854). These are one-sided, nominal 95% simultaneous bootstrap
upper limits, not studentized, basic-bootstrap or two-sided intervals. Thirty-two
heavy-tailed replicas do not guarantee exact simultaneous coverage. There are no
near-zero-reference absolute gates in this completed study.

| Dataset | Region | Variance / PBRT | Variance upper | Tail / PBRT | Tail upper |
| --- | --- | ---: | ---: | ---: | ---: |
| day/raw | full | 0.454723 | 12.3163 | 0.451286 | 12.2232 |
| day/raw | wall | 2.19893 | 59.5588 | 19.0908 | 517.081 |
| day/raw | soffit | 217.999 | 5904.59 | 7879.96 | 213432 |
| day/raw | floor | 1.44912 | 39.2499 | 6.4419 | 174.481 |
| day/raw | pool | 0.208307 | 5.64208 | 0.207278 | 5.6142 |
| day/raw | glass | 0.819098 | 22.1856 | 0.818697 | 22.1747 |
| day/clamped | full | 1.09704 | 29.7139 | 1.43947 | 38.9885 |
| day/clamped | wall | 1.1299 | 30.6037 | 1.00792 | 27.3 |
| day/clamped | soffit | 0.642337 | 17.3979 | 1.30008 | 35.2131 |
| day/clamped | floor | 1.03254 | 27.9667 | 1.15911 | 31.3949 |
| day/clamped | pool | 1.34079 | 36.3159 | 3.72024 | 100.764 |
| day/clamped | glass | 0.96386 | 26.1065 | 1.53771 | 41.6494 |
| night/raw | full | 0.658602 | 17.8385 | 0.648732 | 17.5711 |
| night/raw | wall | 0.556315 | 15.068 | 0.279442 | 7.56878 |
| night/raw | soffit | 0.972773 | 26.3479 | 0.907676 | 24.5847 |
| night/raw | floor | 41.3981 | 1121.28 | 44.4978 | 1205.24 |
| night/raw | pool | 1.10958 | 30.0533 | 1.10927 | 30.0449 |
| night/raw | glass | 0.874875 | 23.6963 | 0.931754 | 25.2369 |
| night/clamped | full | 0.871827 | 23.6137 | 0.987111 | 26.7363 |
| night/clamped | wall | 0.657199 | 17.8005 | 0.523157 | 14.1699 |
| night/clamped | soffit | 1.21483 | 32.9042 | 1.50673 | 40.8104 |
| night/clamped | floor | 0.283047 | 7.66644 | 0.373284 | 10.1105 |
| night/clamped | pool | 1.28581 | 34.8266 | 1.19308 | 32.3151 |
| night/clamped | glass | 0.942151 | 25.5185 | 1.08612 | 29.418 |

The ten failed night mean safeguards are full-frame RGB, soffit R, floor RGB and
glass RGB. Full-frame means are 5.44%, 8.63% and 6.34% below PBRT; floor means
are 32.65%, 33.84% and 37.46% below; glass means are 17.08%, 15.81% and 13.06%
below. Soffit R is 3.41% above PBRT and exceeds its fixed allowance by a factor
of 1.00265. Its narrow failure is retained. The lower night clamped variance
cannot be interpreted as equal-quality improvement while those means differ.

The largest raw point gaps require further diagnosis: day soffit variance/tail
are 218.0×/7,879.96× PBRT and 48.82×/363.93× the merged baseline; night floor
variance/tail are 41.40×/44.50× PBRT and 3.78×/5.21× the merged baseline.
These are measured finite-study differences, not an attribution of their cause.
The regression and exact full-image export checks pass independently of these
failed scene-level checks. PR readiness remains held for mean/outlier diagnosis.

The independent audit recomputes all point variances, positive leave-one-out
tails, epsilons, RGB means, guard thresholds and reported-bound arithmetic. It
agrees with the native report within its fixed tolerances. It authenticates the
unchanged bootstrap source, executable and input manifest; it does not regenerate
the 20,000 bootstrap maxima. The original failed studies remain preserved.

### Separate repeated timing

The fixed 320×170 / 64-spp workload uses seed/material seed zero, one warmup and
three measured runs per scene/version, serially on the M4 Max. All 16 runs
complete. These whole-process wall times include both output planes and durable
checkpoint export. Values are mean ± sample standard deviation, not confidence
intervals. The comparator is frozen v5, renderer 14/sampling 7, not merged main.

| Scene | v5 seconds | v8 seconds | v8 / v5 |
| --- | ---: | ---: | ---: |
| Day | 52.063 ± 0.131 | 117.714 ± 0.293 | 2.261× |
| Night | 57.946 ± 0.104 | 171.566 ± 0.914 | 2.961× |

This substantial slowdown is separate from the primary 256/512-spp quality
budgets. These measurements do not establish a time-to-quality curve or predict
full-resolution throughput. Both full-resolution images and their exact export
audits are recorded in the [regression record](transport-regression-validation.md).

Evidence is retained under `.cache/pavilion-convergence/final-v8/`:

| Record | SHA-256 |
| --- | --- |
| `quality-metrics.json` | `a24b609a251382b9f87825125a536939b4fc131f9c034c407f2e20071c70a93a` |
| `full48-evaluation.json` | `23e9b50993f3f1523bd4bc7e1661416dcb7a35610fbc45392c2966cf764e124d` |
| `independent-final-review-001/audit-result.json` | `719b7aa52edc2e99feef24d9f9f40da888a6251f96db5a88edaf98514c36db3e` |
| `report-plots-001/outputs/receipt.json` | `99938f18a0e9f40b5f3ccd3e7de701a0187f57f74786032efbbba473d19bda95` |

The report receipt binds every timing record and all 48 plotted endpoints;
failed endpoints are retained. Point estimates and one-sided upper limits are
labelled separately. New diagnostic work must not replace this failed dataset,
change its thresholds or relabel it as accepted.

## Persistence and resource boundaries

Current format/identity/renderer/sampling/accumulation contracts are
**3/12/20/7/2** after the full-sphere clipping repair and native-triangle hint
integration. The completed scene study
above remains frozen at renderer 18; the earlier light-index measurements below
use renderer 14.
Format 3 is required because checkpoint metadata now stores the separate signed
material seed. The identity includes that seed and light policy. Sampling
contract 7 rejects the earlier contract-6 tiled Halton light stream and
intermediate contract-5 coating stream before continuation.
Older checkpoints are rejected before payload decoding; same-build batch and resume
checks remain exact. Cross-revision pixel identity is not expected after these
proposal changes. Large assets and retained render artifacts stay outside Git.

## Frozen quality protocol

[The machine-readable protocol](../tests/corpus/pavilion-quality.json) was frozen
before candidate Pavilion measurements. Its SHA-256 is
`6657ac1a3a8ef296f6fbcf7ef58190e35374e210983db03514a06d9e28cdd99d`.
Scenes and all 159 dependencies come from the
[pinned acquisition manifest](../tests/corpus/pbrt-pavilion.json), including the
sibling landscape dependencies. Variants retain camera, world, filter and ISO.

At 320×170, each backend uses 32 complete-image replicates: candidate seeds
0–31, pinned CPU PBRT 32–63, frozen baseline 64–95. Candidate/PBRT material seeds
match their sampler seeds; the baseline keeps its historical behavior. Day
budgets are 64 and 256 spp; night budgets are 64 and 512 spp. Both raw and
originally clamped planes use FLOAT linear output. PBRT's raw companion changes
only Film clamping and storage precision, preserving ISO; known sensor exposure
is divided out after decoding.

The primary day-256 and night-512 measurements cover the full image and five
fixed regions. Variance is mean unbiased per-pixel luminance variance. Tail
energy uses the largest positive squared leave-one-out residuals in 0.1% of
region pixels, divided by all region pixels, then averaged across replicates.
The 20,000-draw bootstrap resamples whole images independently by backend and
jointly across planes/regions. It recomputes means, residuals, ranks and reference
floor on every draw. Acceptance requires all 48 simultaneous one-sided 95%
upper bounds to be at most **1.25× PBRT**, with an absolute energy gate near zero.
Raw RGB regional means retain the separate five-combined-standard-error plus
`2e-5 * max(1, abs(reference))` gate. Failed or inconclusive bounds fail;
replicates are not discarded and seed counts are not extended adaptively.

Full images use the authored 1600×850 resolution, day 256 spp, night 512 spp,
and sampler/material seed zero. Timing runs use three serial repetitions at
matched budgets and 16 PBRT threads. Concurrent validation timings are not
performance evidence. CLI preparation measures renderer-resource construction,
separately from pipeline creation. Parsing/translation, identity hashing and
image export remain included in end-to-end wall time, not those two phases.
Unsupported phase telemetry is reported as unavailable.

## Original contract-6 validation evidence

The completed study includes all 384 prescribed backend/budget/seed cases and the
frozen quality analysis, but **does not meet the convergence acceptance target**.
Serial timings and the bounded diagnostics below are complete. Local validation
uses an Apple M4 Max with
128 GiB unified memory, macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang
21.0.0, CMake 4.0.3 and Ninja 1.13.2. The frozen candidate's production-source
digest is `dc2b7b3b30c89150a9fc4af3f470544c06c71b1d5b84b65bd1838d5e77739a63`;
its Release executable SHA-256 is
`d14df941174cb5f3e92cdd91adb330ac96bddff1619fc8bc15ef0f017f249e49`.
That frozen candidate completed the following checks:

- Debug, Release and ASan/UBSan: 211 tests each, no failures. A separate
  Metal-disabled build passes all 153 portable tests with no skips.
- Metal API validation passes all 60 native tests on the M4 Max.
- All 225 Python policy/harness tests pass. The CI Clang-Tidy job passes;
  the Metal host adapter passes Apple Clang Static Analyzer.
- Eight real SIGINT/SIGTERM checks cover packet path/BDPT, independent/Sobol
  sampling and material seed `INT_MIN`. Cancellation preserves the final-image
  sentinel; resumed batches preserve all 216 bytes of per-pixel state exactly.
  Four completed-checkpoint exports also pass without an adjacent metallib.
  A genuine contract-5 checkpoint is rejected without changing its output sentinel.
- All unchanged 45 coating and 150 broader packet comparison gates pass, with
  every prescribed replicate retained.
- The 32×24, 128-spp legacy demo retains exact PNG bytes and raw pixel state
  against the merged baseline.

- Native environment sampling: 65,536 histogram samples, 36 frequency gates and
  1,376,256 stress proposals, including 8192-square maps and reflected transforms.
- Native incident geometry: 81,920 proposals across 20 cases. Portable tests use
  independent 512-square area quadrature, bilinear moments and eight fixtures
  generated from the pinned Float PBRT library.
- Light selection/emission probes: forward/reverse agreement for three policies,
  and sampled/queried emission PDFs plus independent sphere integration for four
  positional types under identity and affine transforms.
- Coating hash/PCG oracles: 20 hashes and 48 PCG outputs. Native moment checks use
  1,572,864 samples across 12 diffuse/conductor, transport-mode, depth and angle
  cases; 60 corresponding-route gates pass their preselected tolerance.

The corrected candidate's three diagnostic coating configurations also pass all
135 unchanged gates (three configurations × three cases × 15 gates), retaining
all 144 images.
Restoring the old fixed-PDF/event-value policies does not reproduce the larger
historical deficit while the new joint walk and light selection are held fixed.
Both isolated switches change all 16 coated-BDPT images; their conditional mean
effects stay below 0.05% of the PBRT mean. All 48 diagnostic path-control images
are byte-identical to production: that constant-environment control exercises the common sampled
walk, not directional value/PDF evaluation. These controls do not identify a
single cause for the historical deficit, and they do not remove PBRT's finite
approximation or its previously measured difference between estimator routes.

The corrected native probes pass 10 scalar and 28 packet fixed-input fixtures,
including signed material seeds, rejected entrances, deep roulette, diffuse-map
boundaries and an exact horizontal phase proposal. The standalone
[PBRT oracle and reproduction instructions](../scripts/README-layered-schedule.md)
reproduce all 28 expected rows exactly against the pinned unmodified build.
The existing scalar
11×8,192-sample comparisons and all 60 packet moment gates also pass unchanged.
Retained negative controls expose each individual correction. A later test-only
probe update leaves the executable and all 34 other complete linked Metal
modules byte-identical, including their helpers and metadata; this establishes
unchanged compiler inputs for the measured renderer, not cross-toolchain identity.

The independent review caught a constant-environment escape weight that would
have returned `1 - log(17)/16` instead of unity for an unoccluded Lambertian
bounce; its queried PDF now matches the absent NEE strategy. It also caught
stale retained-memory accounting for the native environment CDFs; allocation
validation now charges native tables. A reflected-disk bound also applied an
extra handedness flip. The new invisible-emitter regression returns zero on the
retained faulty build where the independent irradiance is `1/37`; corrected
bounds pass both orientations, both receiver sides and visible/invisible cases.
The Pavilion quality failure below remains separate from these passing component
and compatibility checks; they do not establish scene-level convergence parity.

## Primary Pavilion quality result

All 32 prescribed replicates per backend and budget are retained at 320×170.
At day 256 spp and night 512 spp, **0/48 simultaneous quality bounds pass** the
frozen 1.25× limit; **36/36 raw RGB mean checks pass**. Twenty quality point ratios
exceed 1.25; the remaining 28 have point ratios within the limit but inconclusive upper
bounds. A failed upper-bound test does not establish that every population metric
is worse. Mean agreement under these gates does not prove estimator equality.

The common simultaneous log margin is `3.2958833308443456`, multiplying each
point ratio by `27.001254579827147` for its one-sided upper bound. The table gives
all primary point ratios, including regressions against the merged baseline.
These are descriptive estimates from the fixed ensemble, not accepted parity
claims. Film clamping is nonlinear, so raw and clamped variance orderings can
differ; neither plane substitutes for the other.

| Scene/plane | Region | Variance / PBRT | Tail / PBRT | Variance / baseline | Tail / baseline |
| --- | --- | ---: | ---: | ---: | ---: |
| day/raw | full | 0.48895 | 0.48572 | 1.637 | 1.6236 |
| day/raw | wall | 1.1365 | 1.1569 | 0.021001 | 0.014671 |
| day/raw | soffit | 223.23 | 8069.4 | 49.989 | 372.68 |
| day/raw | floor | 2.3652 | 18.023 | 0.06238 | 0.49288 |
| day/raw | pool | 0.31314 | 0.31199 | 1.5432 | 1.5543 |
| day/raw | glass | 0.86763 | 0.86763 | 2.8951 | 2.8971 |
| day/clamped | full | 1.0905 | 1.4166 | 0.05582 | 0.12476 |
| day/clamped | wall | 1.1237 | 0.91219 | 0.022743 | 0.041281 |
| day/clamped | soffit | 0.68007 | 2.5118 | 0.15457 | 0.13036 |
| day/clamped | floor | 1.0641 | 1.56 | 0.029102 | 0.050758 |
| day/clamped | pool | 1.3784 | 3.825 | 0.51418 | 0.99774 |
| day/clamped | glass | 0.964 | 1.6328 | 0.2392 | 0.1805 |
| night/raw | full | 0.64046 | 0.63008 | 0.777 | 0.77916 |
| night/raw | wall | 1.7021 | 0.44217 | 0.0091423 | 0.001318 |
| night/raw | soffit | 0.976 | 0.90169 | 0.135 | 0.29824 |
| night/raw | floor | 0.20047 | 0.055594 | 0.018295 | 0.0065073 |
| night/raw | pool | 1.4663 | 1.4664 | 0.86296 | 0.86709 |
| night/raw | glass | 0.93102 | 0.93562 | 0.41697 | 0.56273 |
| night/clamped | full | 1.3594 | 1.3543 | 0.56073 | 0.46058 |
| night/clamped | wall | 2.1284 | 1.1913 | 0.28813 | 0.047163 |
| night/clamped | soffit | 1.3505 | 1.4925 | 0.34325 | 0.69991 |
| night/clamped | floor | 2.1585 | 3.0241 | 3.4107 | 1.0053 |
| night/clamped | pool | 1.0823 | 1.1572 | 0.89553 | 1.152 |
| night/clamped | glass | 1.1604 | 1.2354 | 0.6692 | 0.74662 |

For the displayed Film plane in this 320×170 ensemble, full-image variance is
94.4% below the merged baseline by day and 43.9% below it at night. Relative to
PBRT, the night variance is 1.3594× overall, 2.1284× on the wall and 2.1585× on the floor: approximately
17% and 46–47% more RMS noise, respectively. These ensemble estimates do not
measure the individual full-resolution images below. The night floor also
regresses to 3.4107× the baseline variance. Improvements elsewhere do not erase
that result.

A descriptive audit of every retained day-256 raw EXR exactly reproduces the
soffit failure. Seed/material seed 25, pixel `(119,48)`, has raw luminance
`4.2238812054` and contributes 94.3142% of regional variance and 97.6237% of tail
energy. The leading three seed/pixels account for 96.6002% and 99.9899%,
respectively. No image or contribution is excluded, and no acceptance gate is
recomputed after removal.

A subsequent owner-local replay preserves the original resolution, total sample
budget, pixel, sampler/material seeds and frozen metallib. All 256 contributions
reconstruct the retained raw FLOAT RGB exactly. Sample 171 contributes 99.8956%
of that pixel's pre-export luminance. A host-only observer captures every guarded
workspace and all 80 bytes of each packet result; all 256 captures agree with
the decoded packet fields and pass their guards. No shader arithmetic changes
in this replay. Receipts remain under
`.cache/pavilion-convergence/final-v4/soffit-replay/workspace-observer/`.

A separately instrumented single-sample trace then passes exact 80-byte result
identity, guards and trace bounds. All ten ordered connection contributions sum
back to the original result in binary32. The sole nonzero contribution is the
`(s=0,t=6)` environment escape after diffuse, delta transmission, coated reflection
and another delta transmission. The transmission throughput factors `4/9` and
`9/4` cancel; final spectral throughput is about 0.081–0.210. The final delta event
correctly bypasses directional MIS, giving weight one; pinned PBRT's vacuum
volpath applies the same conditional policy after a specular bounce. The coated-surface NEE
connection is occluded. This identifies a rare refracted-environment caustic in
this day sample, without identifying a MIS/eta amplification error. It does not
establish universal transport correctness or explain the broader night variance
gap. Authored surface names are not inferred from raw GPU IDs. The distinct trace
sources, oracle, records and strict decoder remain under
`.cache/pavilion-convergence/final-v4/soffit-replay/path-trace/`.

An execution interruption occurred after 353 complete statistical receipts.
All completed receipt hashes remained unchanged; the unfinished case reran with
the same prescribed seed, with its incomplete files archived. This was
infrastructure recovery, not replacement of a failed statistical replicate.
The retained primary result is
`.cache/pavilion-convergence/final-v4/quality-metrics.json`, SHA-256
`19dce8c4f82b935f44b5cb924e883c2257ef5f802bd7b0cf92b38c64e0922ec9`.

## Controlled Halton-layout diagnostic

A separate protocol was frozen before new measurements, SHA-256
`61712cb1c82262009fee6dbfd798c05ceecae218f85aeaad37edb21db9b95d85`.
It compares the current layout, a compact depth-sized layout and pinned PBRT at
320×170/night 64 spp, using all 16 new sampler/material seeds 96–111 per arm.
Only Halton BDPT light-walk, direct-light and splat dimension ranges change;
camera/endpoint variates, transport arithmetic and material algorithms stay fixed.
The compact layout is a diagnostic adaptation; PBRT uses a branch-dependent
sequential dimension cursor. The [Halton sampling background](https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Halton_Sampler)
does not establish a cause for this already-scrambled scene sampler's noise.
The full-reference control and both independent-sampler negative-control pairs
are decoded-pixel exact. Experimental runs are fresh-only and cannot resume
production checkpoints.

The 20,000-draw paired whole-image bootstrap uses seed `1380798801` and four
predeclared wall/floor Film-clamped endpoints. Positive evidence requires both
variance point ratios at most 0.80, their simultaneous upper bounds below 1,
neither tail upper bound above 1.25, and passing raw mean safeguards.

| Endpoint | Compact / control | Simultaneous 95% upper |
| --- | ---: | ---: |
| wall variance | 0.942795 | 1.294744 |
| wall tail | 0.777122 | 1.067225 |
| floor variance | 1.069628 | 1.468924 |
| floor tail | 1.093033 | 1.501067 |

The predeclared positive-evidence rule is **not met**. All 54 pairwise raw RGB
mean safeguards pass, but the variance estimates move in opposite directions
and the bounds remain inconclusive. Full-image clamped variance/PBRT is 1.3169
for control and 1.3313 for compact. This result neither establishes this layout
as the cause of the night gap nor rules out other sampler effects. Production
retained the original dimension layout. The separate full-Film light-index
correction below advances the sampling contract to 7.

All 48 main cases, four independent-sampler controls and one direct frozen
reference are retained. An initial staging-path failure occurred before any
render; replacing external symlinks with verified hardlinked assets and copied
scene sources preserved the reviewed runner and all seeds. No measured sample
was replaced. The result, reviewed inputs and recovery are bound by
`.cache/pavilion-convergence/halton-layout-diagnostic/final-receipt.json`, SHA-256
`116efd0adb3e7c756c241a4bdd81ebbe526e5e444113fe2a6ea2eb9f09e13705`.
This diagnostic is separate from the unchanged primary acceptance study.

## Reference Film and sampler audit

The retained PBRT EXRs use the same authored resolution and camera sample
budgets: 1600×850, day 256 spp and night 512 spp. All 256 controlled reference
EXR headers also match the 128 case receipts and their 320×170 budgets. Both
renderers use subpixel Halton samples and the default Gaussian reconstruction
filter, with radius 1.5 pixels and sigma 0.5. The statistical comparison reads
FLOAT EXRs directly, without denoising, sharpening or downsampling. PBRT's
8-bit PNG writer adds blue-noise quantization dither; this cannot account for
the measured FLOAT variance. A separate CPU audit also reconstructs every
channel of both actual full-resolution reference PNGs exactly from their FLOAT
EXRs using pinned PBRT sRGB encoding, blue-noise dither and endpoint clipping:
4,080,000 matching bytes per image. The PNGs contain only IHDR/IDAT/IEND chunks,
without color-profile or gamma metadata. This establishes pixel consistency;
the historical conversion commands remain unknown, and viewer interpretation
is separate. The audit is retained under
`.cache/pavilion-convergence/final-v5/reference-png-audit/`, with result SHA-256
`7a84aaef9738119e26d3b3971450af6181248bc47dd60da08667f08044ee6029`.

Source review finds the same Film grouping and normalization: clamp the exposed
aggregate camera contribution once, clamp each light-path splat separately,
normalize camera sums by reconstruction weight and splats by sample count times
filter integral. Small Gaussian evaluation precision differences remain. These
checks identify no substantial Film scaling or extra-supersampling explanation.

The Halton core also uses the same global digit permutations, pixel/sample index,
wavelength dimension and primary-lens dimensions. The assignment of later
dimensions differs: rypath uses fixed role ranges, whereas PBRT advances a
branch-dependent cursor. Generic diffuse scattering, area-light emission and
environment launch also use polar disk maps in rypath and concentric maps in
PBRT; their intended densities agree, but their interaction with Halton samples
can differ. The coated diffuse base already uses the corrected concentric map.
The disk-map difference remains an unmeasured hypothesis. The fixed light roles
also expose the periodic-index duplication demonstrated below.

PBRT additionally propagates texture differentials through specular camera
events, while rypath switches to its camera approximation after the first hit.
This documented approximation can affect textures seen through reflection or
refraction. It does not affect first-hit texture filtering, and the traced day
outlier starts with diffuse scattering, which also discards PBRT's differentials.

## Halton light-path reuse across Film tiles

An eight-replicate diagnostic at 320×170/night 64 spp separates camera sums,
light splats and their covariance. It uses candidate seeds 128–135 and PBRT
160–167, with matching material seeds, all six frozen ROIs and both output
planes. Checkpoint decomposition reconstructs candidate totals; a post-render
PBRT Film observer preserves canonical output bits. These are descriptive
diagnostics, separate from the retained 32-replicate acceptance study.

For the clamped plane, light-splat variance accounts for 92.4% of the full-image
candidate-minus-PBRT variance, 115.4% in the wall ROI and 100.6% in the floor ROI.
Shares above 100% reflect offsets from camera variance and covariance. Changing
only the sampler to independent reduces total candidate/PBRT variance ratios
from 1.304 to 0.920 overall, 1.780 to 0.943 on the wall, and 2.247 to 0.961 on
the floor. This localizes a sampling interaction; it does not establish parity.

The Halton camera index repeats its pixel offset every 128 pixels in each axis.
That reuse is unsuitable for fixed-role BDPT light paths whose splats land
throughout the Film. A frozen native probe traces six source pixels separated
by 128 pixels, for each of 64 absolute samples. Three positive splat events
repeat bit-for-bit across all six owners: 18 contributions and 45 positive
pair-event matches. Camera contributions differ. The corrected probe has zero
such matches, with all 384 wavelength/PDF arrays unchanged. This demonstrates
removal of a concrete repeated-output pattern, not independence of every path.

The correction keeps the camera index and fixed dimension roles. For Film tile
count `T = ceil(width/128) * ceil(height/128)`, reserve `S = bit_ceil(T)` slots
and evaluate the existing CRT index at absolute light sample `n*S + tileId`.
Only light emission, the importance walk and splat-lens draws use that index.
Camera walks, wavelengths and camera-side direct-light draws retain theirs.
There is still one light path per source owner: PDFs, MIS, Film normalization,
clamping and contribution weights are unchanged. Shared wavelengths and
camera-inherited wavelength termination remain coupled.

Power-of-two slots are deliberate. Multiplying by `T` itself would freeze a
leading radical-inverse digit whenever `T` shares a light dimension's prime
(for example, 23 tiles and dimension 8). Power-of-two slots are coprime to all
light primes, which are at least 5. Both axes at most 128 retain the old index
exactly. The sequence depends on full Film dimensions and absolute sample,
independently of batches, dispatch partitioning or target sample count.
Host admission bounds the expanded index before multiplication and before
progressive GPU preparation. Sampling contract 7 prevents mixed-stream resume.

The production contract-7 candidate is frozen separately under
`.cache/pavilion-convergence/candidate-v5/`, with source digest
`31fd7542c213a84b7ffe024966e78312f8b25842264ff1e5b62a84f8d4a93054`,
executable SHA-256
`09b5baae1a23702854c29ad6d9e4f9ab1dbe5ae550b538043608d5ccf8903621`
and metallib SHA-256
`c07cccd790cc4da98842af5d0f58c09020e3093fdc2979395fe972d58a8bf75e`.
It was frozen before commit `c6efc88`; the embedded revision still names
`b3269aa`, while every archived production-source hash matches the committed
correction. Source hashes and the immutable binary manifest bind these renders.
Current validation on the same M4 Max covers all 211 Debug, Release and
ASan/UBSan checks, 225 Python tests, five affected Metal API validation tests,
formatting, focused Clang-Tidy and the native adapter's static analysis.
The first Debug/Release passes exposed two contract-6 golden digests; independent
Python serializers reproduced the old values and generated the contract-7
expectations, after which both failed tests passed. No renderer changes were
needed. New native regressions cover positive splat diversity, partition and
threadgroup invariance, 32-bit sample carry, early capacity rejection and exact
serialized resume across a tile boundary, including every raw/clamped Film plane.

The fixed eight-seed correction pilot completes all six preflight renders and
eight exact FLOAT-plane comparisons: unchanged control versus the archived
candidate, independent-sampler and 96×51 Halton negative controls, and the
production candidate versus the isolated corrected shader. Seed 128 is the
predeclared first ensemble member and is reused from the production-equivalence
check. All eight prescribed images complete, and all 18 paired raw RGB mean
safeguards pass; independent recomputation reproduces every variance statistic exactly.

| Clamped ROI | Corrected / original variance | Corrected / PBRT variance |
| --- | ---: | ---: |
| full | 0.725609 | 0.946364 |
| wall | 0.289698 | 0.515662 |
| soffit | 0.874896 | 0.976082 |
| floor | 0.316908 | 0.711937 |
| pool | 1.030400 | 1.047736 |
| glass | 0.882755 | 0.979329 |

The wall/floor reductions demonstrate a material effect of removing the repeated
light samples. Pool variance increases by 3.0% in this pilot. Raw full-image
variance is almost unchanged (corrected/original 0.998422); the large camera-path
contributions remain. Eight replicas at 64 spp supply descriptive localization,
not simultaneous confidence bounds, tail acceptance, performance evidence or a
parity claim. The result is retained at
`.cache/pavilion-convergence/variance-gap-investigation/light-index-diagnostic/pilot-analysis.json`,
SHA-256 `d0b4c3f939e701552a2d338b309678c4948af02d6ce82424eb4c9df1f9d46bd8`.
The final pilot receipt is SHA-256
`a7b74f602a533818743a0291b396e825bbfcb1a7dbc63377182594d52843155a`.

## Contract-7 night re-evaluation

The frozen corrected candidate completes the prescribed 32 night replicas at
320×170 and 512 spp: candidate seeds 0–31, retained PBRT seeds 32–63 and retained
baseline seeds 64–95, with matching material seeds where supported. All 96
receipts and 192 FLOAT EXRs are authenticated. The original protocol, regions,
scaling, tail definition, 20,000 whole-image bootstrap draws and 1.25 limit
remain unchanged. Calibration covers the 24 selected night variance/tail
endpoints jointly; this is a separate night-only re-evaluation, not completion
or acceptance of the original 48-endpoint day/night study.

All **18/18 raw RGB mean safeguards pass**, but **0/24 simultaneous upper bounds
pass**. The common log margin is 2.9049269533 (an upper-bound multiplier of
18.2639). Eighteen of the 24 point ratios are at most 1.25; point estimates alone
do not satisfy the confidence requirement. Independent recomputation verifies
every point variance, positive leave-one-out tail, epsilon, mean guard and bound
decision. Bootstrap source/binary/input provenance is checked; the independent
reviewer does not regenerate the 20,000 bootstrap maxima.

Authored-clamped output:

| ROI | Variance / PBRT | Variance upper bound | Tail / PBRT | Tail upper bound |
| --- | ---: | ---: | ---: | ---: |
| full | 0.897602 | 16.393730 | 0.990737 | 18.094722 |
| wall | 0.632463 | 11.551254 | 0.451561 | 8.247263 |
| soffit | 1.188816 | 21.712422 | 1.495126 | 27.306844 |
| floor | 0.611318 | 11.165063 | 0.516555 | 9.434309 |
| pool | 1.080103 | 19.726897 | 1.152576 | 21.050553 |
| glass | 0.970179 | 17.719265 | 1.088815 | 19.886024 |

Unclamped, unexposed raw output:

| ROI | Variance / PBRT | Variance upper bound | Tail / PBRT | Tail upper bound |
| --- | ---: | ---: | ---: | ---: |
| full | 0.697986 | 12.747948 | 0.691261 | 12.625123 |
| wall | 0.535275 | 9.776223 | 0.256269 | 4.680468 |
| soffit | 0.964201 | 17.610081 | 0.900495 | 16.446560 |
| floor | 1.823148 | 33.297819 | 1.922765 | 35.117208 |
| pool | 1.462357 | 26.708349 | 1.462349 | 26.708205 |
| glass | 1.008947 | 18.427311 | 1.280730 | 23.391138 |

Against the same-seed original candidate, clamped variance decreases by 34.0%
overall, 70.3% on the wall and 71.7% on the floor. The corresponding raw results
are mixed: full-image variance increases 9.0%, and floor variance and tail energy
increase 9.09× and 34.59× respectively. Raw pool variance remains 1.462× PBRT,
and the clamped soffit tail is 1.495× PBRT. The repeated-light correction improves
the authored-clamped image but does not resolve raw heavy tails or establish
convergence parity. No failed seed is replaced and no threshold is relaxed.

The two GPU jobs ran concurrently for quality validation, so their durations
are not performance measurements. The retained analysis is
`.cache/pavilion-convergence/final-v5/quality-metrics.json`, SHA-256
`3f530da211df2d6c7cb57d16b5a6eee2c71c71c6e7d8b787c29d5b1f50075763`.
The scoped evaluation remains failed; the native report also retains
`complete=false` and `accepted=false` because the original 48 endpoints are not
present. The independent statistical audit is SHA-256
`5977e24d3f031cdec25522363e10301805b47fa99570930d13d61002ce34c74e`.

## Earlier renderer-14 full-resolution artifacts

The renderer-14 renders complete both authored 1600×850 scenes: day at 256 spp
(348,160,000 camera samples) and night at 512 spp (696,320,000). Each renderer-14
raw and authored-clamped FLOAT EXR matches independent checkpoint reconstruction
at zero ULP across all 4,080,000 RGB components. Each native PNG also matches
all 4,080,000 independently reconstructed channel bytes. Checks validate every
checkpoint plane, compensation pair, sample count, checksum, contract, provenance
segment, effective scene setting and the 159-asset closure. Neither image is
denoised. These concurrent runs supply quality artifacts, not timing comparisons.

Exact historical day identity does **not** pass: four of 1,360,000 pixel records
differ from the contract-6 render, comprising 11 raw and 12 clamped RGB components;
two PNG channels at one pixel differ by one byte level. The largest raw absolute
difference is 0.000146933. Both day versions use identical effective `volpath`
settings, seeds and batches, so the BDPT light-index branch is inactive. All splat planes
and reconstruction weights are identical. The failed historical identity check
is retained separately from the passing renderer-14 output reconstruction checks.

A bounded native replay retains all 256 samples for the four differing owners
and four fixed neighboring controls, with two fresh processes per frozen version.
All 16 unchanged-control pixel records match their full checkpoints exactly.
The two original-version runs match all 2,048 packet records; the two corrected
runs differ in one radiance record, with wavelength/PDF/filter fields unchanged.
One additional sample differs between versions. Overall, 27/32 retained pixel
records and 54/64 FLOAT comparisons reproduce their respective full images.
This demonstrates limited repeatability variation across fresh native scene
builds; it does not isolate shader code generation versus native intersection
behavior or explain every changed full-image owner. No measured run is replaced.
The exact comparison is retained under
`.cache/pavilion-convergence/final-v5/day-repro-probe/comparison.json`, SHA-256
`b4d39726114e3c4d4a9339e2db68b51c0876bfb296afec9e24d4646e35f14447`.

A source-text audit initially rejected equivalent bracket spacing introduced by
scene staging. A bracket-aware lexer resolves that harness error while retaining
semantic comparisons and explicit sample-budget/seed/FLOAT checks; the original
failure is archived. Renderer-14 artifacts and independent audits are retained under
`.cache/pavilion-convergence/candidate-v5-full/` and
`.cache/pavilion-convergence/final-v5/full-artifact-audit/`.

| Artifact | SHA-256 |
| --- | --- |
| `day.png` | `65c500ad1bf66253b639e436fcb11e6a0255cb21c499103987011626201dd9f5` |
| `day-raw.exr` | `891f5ec1677fb322e1c38993129dfde7804ebe371eba701aef536b35212dbbe7` |
| `day.rycp` | `db8e5caf5a983f6fd98abbbb611c83b32bdacbfb4d9aa2dc9bb727c942bf3c05` |
| `day-clamped.exr` | `2456fa966d87100bc6ab73fc990d02350bf9bfa2bf4331ecb252a074f8d10328` |
| `night.png` | `464e9b0ad6af86019d74c8982d60d6c3bbac3b55613e2bb3dca2c80e9f75cdd4` |
| `night-raw.exr` | `f850b8e7b82d904539c9f1f76077ca48d3a54e183612c666d7e72bd937c6b5be` |
| `night.rycp` | `cacd267480a697718d64e2af8015e5c8609faa701fe8a9b63433375dc3f87545` |
| `night-clamped.exr` | `efab8c2bcd9df821b4696071add6d2ac4c57773ea0b255f3453f4324a22cb620` |

Independent `night-audit.json` SHA-256:
`a7282b6c85b8029b2548ad2e598b63f9ec3928b4c1039bc52e575c26b78cd892`.

Independent `day-artifact-audit.json` SHA-256:
`3bbcdfdf2e18247e2a9d7ca1e4e52acea95764df56307102c4fb630973dc786c`.

## Original serial timing and time to quality

All 12 backend/scene/budget cases complete one warmup and three measured runs,
serially on the same M4 Max. The table reports end-to-end seconds, mean ± sample
standard deviation. PBRT uses 16 CPU threads. Candidate and baseline write both
planes in one process; PBRT requires separate raw and clamped companion processes,
shown separately rather than summed when plotting each plane's cost.

| Scene | spp | Candidate, both planes | Baseline, both planes | PBRT, clamped | PBRT, raw |
| --- | ---: | ---: | ---: | ---: | ---: |
| day | 64 | 15.645 ± 0.034 | 15.188 ± 0.048 | 3.880 ± 0.211 | 3.852 ± 0.062 |
| day | 256 | 38.086 ± 0.033 | 37.278 ± 0.045 | 7.622 ± 0.109 | 7.892 ± 0.344 |
| night | 64 | 22.250 ± 0.023 | 22.668 ± 0.026 | 6.713 ± 0.192 | 6.705 ± 0.089 |
| night | 512 | 121.490 ± 0.080 | 127.708 ± 0.536 | 37.212 ± 1.973 | 38.695 ± 0.984 |

At the primary budgets, the candidate is 2.2% slower than the merged baseline by
day and 4.9% faster at night. PBRT's clamped process is 5.00× faster than the
candidate by day and 3.26× faster at night in this 320×170 study. The candidate's
quality improvements over baseline therefore do not establish performance parity
with PBRT. No interpolation of two budget points establishes time to an unmeasured
quality target, and these timings do not predict full-resolution throughput.

Candidate primary-budget resource preparation takes 2.203 s by day and 2.243 s
at night; pipeline creation takes about 0.0015 s, and the render phases take
30.054 s and 113.403 s. Remaining wall time includes scene processing, identity
work and export. Corresponding phase telemetry is unavailable for the baseline
and PBRT. Process peak RSS is about 12.38 GiB for the candidate, 12.35 GiB for the
baseline and 2.64–3.02 GiB for PBRT across these cases. These are process RSS
measurements, not total system/device memory accounting.

Retained raw and Film-clamped plots connect the two measured budgets for each
backend. Their quality points use all 32 fixed replicates; horizontal error bars
show ±1 timing standard deviation, not quality confidence intervals. Plot and
input hashes are retained under
`.cache/pavilion-convergence/final-v4/plots/receipt.json`.
The complete checked report is
`.cache/pavilion-convergence/final-v4/final-evidence.json`; it binds the 48 quality
bounds, 36 raw mean gates, all timing cases, backend identities and full images.

## Original contract-6 full-resolution artifacts

Both authored 1600×850 images are complete: day at 256 spp (348,160,000 camera
samples) and night at 512 spp (696,320,000). Independent checks validate PNG
checksums and decoding, all checkpoint accumulation planes and sample counts,
the pinned asset closure, and authored Film settings. Raw and clamped FLOAT EXRs
match independent checkpoint reconstruction exactly: zero ULP difference across
4,080,000 components per plane, per scene.

An execution interruption stopped the night render at 312 spp. The same frozen
candidate resumed it to 512, retaining contiguous `[0,312)` and `[312,512)`
provenance. The original duration and exit status are unavailable; the resumed
segment's duration is not a full-render timing. These concurrent full-image runs
are excluded from performance comparisons. Completed artifacts and independent
audits remain under `.cache/pavilion-convergence/candidate-v4-full/` and
`.cache/pavilion-convergence/final-v4/full-artifact-audit/`.

| Artifact | SHA-256 |
| --- | --- |
| `day.png` | `43712d092053c8019dc238c8a5be627d40bb3458dcc09ba3c79f82c5d6498e61` |
| `day-raw.exr` | `aadbc313a51805db17eb38718e8245ef8d6841cb7f18daad5f26f940e44bf63b` |
| `day-clamped.exr` | `f6749d8c9ab7189a7cd746d3be65a67353c20f1835c4d1bdb960900d7d0d4e95` |
| `day.rycp` | `eb8ecdeb3aa742e2e6d5d00a406c3fe952427eb732e7e915ba8b8bb21f3c6ce5` |
| `night.png` | `28543f8609dcb4e631047e5aba1c6ff3fdd9531f8a8786f601d5ec30c1e4a376` |
| `night-raw.exr` | `86c894d7f95cf0a8c838980ab4b977871e866c353be6b373379615d7675f85a8` |
| `night-clamped.exr` | `2e50fe5a59a1d45cdbe1dc1e659e76227615856bf03b55c27406eb37d113ad11` |
| `night.rycp` | `043c717edfe3826919c50b9546b2a48e4df41775d831a1f237433d6af6af1d31` |

## Primary references

The implementation is pinned to
[PBRT v4 b0bdf461](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f):
[light samplers](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lightsamplers.cpp),
[lights](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp),
[shapes](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/shapes.cpp),
[layered BSDFs](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/bxdfs.h),
and [integrators](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/cpu/integrators.cpp).
The transport weighting follows
[Veach's multiple-importance framework](https://www.graphics.stanford.edu/papers/veach_thesis/).
The baseline limitations and earlier coating controls remain in the
[historical Pavilion record](pavilion-validation.md).
