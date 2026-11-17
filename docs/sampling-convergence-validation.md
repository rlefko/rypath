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

The corrected candidate's Pavilion ensembles, simultaneous quality bounds and
serial timings are still in progress. Local validation uses an Apple M4 Max with
128 GiB unified memory, macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang
21.0.0, CMake 4.0.3 and Ninja 1.13.2. The frozen candidate's production-source
digest is `dc2b7b3b30c89150a9fc4af3f470544c06c71b1d5b84b65bd1838d5e77739a63`;
its Release executable SHA-256 is
`d14df941174cb5f3e92cdd91adb330ac96bddff1619fc8bc15ef0f017f249e49`.
The corrected candidate completes the following checks:

- Debug, Release and ASan/UBSan: 211 tests each, no failures. A separate
  Metal-disabled build passes all 153 portable tests with no skips.
- Metal API validation passes all 60 native tests on the M4 Max.
- All 225 Python policy/harness tests pass. Changed C++ units pass LLVM 18
  Clang-Tidy; the Metal host adapter passes Apple Clang Static Analyzer.
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
Final convergence, timing and image results must be recorded here
before claiming full acceptance.

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
