# PBRT sampling and Pavilion convergence

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

## Persistence and resource boundaries

Contracts are format/identity/renderer/sampling/accumulation **3/12/14/6/2**.
Format 3 is required because checkpoint metadata now stores the separate signed
material seed. The identity includes that seed and light policy. Sampling
contract 6 rejects the intermediate contract-5 coating stream before continuation.
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

## Validation evidence

The completed study includes all 384 prescribed backend/budget/seed cases and the
frozen quality analysis, but **does not meet the convergence acceptance target**.
Serial timings and the bounded diagnostics below are complete. Local validation
uses an Apple M4 Max with
128 GiB unified memory, macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang
21.0.0, CMake 4.0.3 and Ninja 1.13.2. The frozen candidate's production-source
digest is `dc2b7b3b30c89150a9fc4af3f470544c06c71b1d5b84b65bd1838d5e77739a63`;
its Release executable SHA-256 is
`d14df941174cb5f3e92cdd91adb330ac96bddff1619fc8bc15ef0f017f249e49`.
The corrected candidate completes the following checks:

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
remains on the original measured layout and sampling contract 6.

All 48 main cases, four independent-sampler controls and one direct frozen
reference are retained. An initial staging-path failure occurred before any
render; replacing external symlinks with verified hardlinked assets and copied
scene sources preserved the reviewed runner and all seeds. No measured sample
was replaced. The result, reviewed inputs and recovery are bound by
`.cache/pavilion-convergence/halton-layout-diagnostic/final-receipt.json`, SHA-256
`116efd0adb3e7c756c241a4bdd81ebbe526e5e444113fe2a6ea2eb9f09e13705`.
This diagnostic is separate from the unchanged primary acceptance study.

## Serial timing and time to quality

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

## Full-resolution artifacts

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
