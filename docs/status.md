# Project status

## Current milestone

Pavilion scene support merged in
[PR #50](https://github.com/rlefko/rypath/pull/50) at `dd8682d5`.

The current compatibility slice renders the pinned Barcelona Pavilion day and
night scenes: native sphere/disk/open cylinder, four-wavelength vacuum transport,
BDPT camera/light connections and splats, finite sensor-space film clamping,
raw EXR export, and an explicit bounded large-scene profile. Checkpoint
format/identity/renderer/sampling/accumulation contracts on this follow-up branch
are 3/12/18/7/2.
See the [Pavilion record](pavilion-validation.md) for model limitations, acquisition,
acceptance targets and historical validation. The follow-up adds native environment
CDFs, combined light selection, directional shape/emission proposals and pinned
coating RNG domains; [sampling/convergence validation](sampling-convergence-validation.md)
tracks its current evidence. The original frozen Pavilion study is complete: all 36 raw
RGB mean checks pass, but the 48 simultaneous variance/tail bounds do not meet
the convergence target. Serial timing and bounded diagnostics are complete: a
compact Halton layout did not meet its improvement rule, and an exact replay
identified a refracted-environment caustic behind one day outlier. Night attribution
then demonstrated repeated Halton BDPT light splats across 128-pixel tiles.
The follow-up gives those light paths distinct full-Film indices; its new
variance measurements are separate from the original failed study.
Further [regression diagnosis](transport-regression-validation.md) isolates a
missing importance shading-normal adjoint in the night outlier and inconsistent
near-edge native triangle hits in the day replay. Broader validation exposed
arithmetic, candidate-coverage and boundary-stress regressions; fixed replays
identified their causes. Renderer 18 repairs coverage, range and traversal cost.
The combined candidate passes all 220 checks in each supported preset, including
native API validation. Four fresh Pavilion contexts reproduce the fixed 8,192
packet records and owner reductions exactly. Both fresh 1600×850 day-256 and
night-512 images are complete and independently audited. The completed fixed study
fails: 0/48 simultaneous quality bounds and 26/36 raw mean safeguards pass
(day 18/18, night 8/18). PR #51 remains a draft for night mean-shift and outlier
diagnosis. Formal timing also records slower whole-process execution than the
frozen renderer-14 v5 candidate: 2.261× by day and 2.961× by night.
The earlier pilots and failed quality studies remain historical evidence.
Subsequent fixed night PT comparisons pass all six regional rypath/PBRT mean
checks, while BDPT differs from PT within both renderers. Rypath's remaining
within-renderer failures are floor red and green. This narrows the diagnosis
without establishing PT convergence or replacing the original quality verdict.
Execution-only arithmetic and visibility optimizations are integrated separately;
their combined repeated performance comparison remains pending.
Unsupported media and geometry receive
diagnostics rather than substitutions.

This remains selected-scene coverage. Curves, motion, heterogeneous media,
additional camera/film models and broader material semantics remain on the
[roadmap](roadmap.md). Offline MVP acceptance and full pinned-corpus coverage
remain unestablished.

## Validation

The subsequent execution changes in `d1d205e` pass all 220 Release checks and
all 220 ASan/UBSan checks, plus 158 portable Clang 18 checks. Debug initially
passes 219/220; the new alpha test fixture lacked required texture-frame metadata.
After correcting that fixture, its rerun passes and all 64 native Debug checks
pass with Metal API validation. Focused Clang-Tidy and Apple analyzer checks pass.
These results are distinct from the frozen renderer-18 evidence below and do not
yet establish a combined speedup or improved Pavilion quality.

Renderer-18 candidate `c3489bf` passes Debug, Release and ASan/UBSan validation
(220 checks each) on the M4 Max running macOS 26.6. All 64 native tests pass with
Metal API validation in both Debug and Release, including the original ten
failures and the restored 4,097-plane boundary stress. Focused Clang-Tidy and
Apple analyzer checks pass; the unchanged Python suite passes 225 checks. The
extended eight-seed, 8,192-spp importance-continuation check also passes on an
immutable renderer-18 snapshot preceding only the alpha probe changes. See the
[regression record](transport-regression-validation.md) for revision boundaries,
failure attribution and numerical limits. The fresh 320×170 day-256 and night-512
pilots complete with finite raw and authored-clamped outputs in 301.00 and
1,026.31 seconds. These incidental timings show increased cost relative to the
historical pilots. The separate repeated comparison is complete: at 320×170 and
64 spp, renderer-18 v8 takes 117.714 seconds by day and 171.566 by night versus
52.063 and 57.946 for renderer-14 v5. These are means of three measured runs
after one warm-up per scene/version, not time-to-quality results. Both fresh
1600×850 full images are complete at day 256 spp and night 512 spp. Every raw
and authored-clamped FLOAT EXR component reconstructs exactly from the checkpoint
(0 ULP), and all 4,080,000 PNG channels per image match checkpoint-derived sRGB.
Counts are uniform and all checkpoint accumulation values are finite. These
artifact checks remain separate from the failed quality verdict. The ten failed
night mean safeguards cover full-frame RGB, soffit R, floor RGB and glass RGB.
The largest point gaps versus PBRT remain day raw soffit variance/tail
(218.0×/7,880.0×) and night raw floor (41.40×/44.50×). See the
[regression record](transport-regression-validation.md) for mean shifts, timing
spread and authenticated study evidence.

Published `337c4fd` repairs only initialization in the disabled Metal probe stub;
the native renders retain frozen `c3489bf` source and binaries. Its mandatory
pre-push Debug check passes all 220 tests; a separate upstream Clang 18.1.8
Metal-disabled build on macOS passes 158 tests. All six jobs in
[CI run 44632608875](https://github.com/rlefko/rypath/actions/runs/44632608875)
pass, including Linux, macOS, CodeQL and Clang-Tidy. The
[regression record](transport-regression-validation.md) distinguishes these
publication checks from the frozen rendering evidence.

The earlier renderer-14, sampling-7 light-index correction passed all 211 Debug, Release and sanitizer
checks, 225 Python checks and five affected Metal API validation checks. Its
32-seed night-512 re-evaluation reduces authored-clamped variance by 34.0%
overall, 70.3% on the wall and 71.7% on the floor versus the original candidate.
All 18 raw mean safeguards pass, but none of the 24 simultaneous quality bounds
passes. Raw floor variance rises 9.09×; raw heavy tails and convergence parity
remain unresolved. This separate night-only result does not replace the original
48-endpoint study. Fresh 1600×850 day-256/night-512 outputs reconstruct exactly
from their checkpoints. A documented four-pixel day difference prevents an exact
historical day-identity claim.

The earlier contract-6 candidate passed Debug, Release and ASan/UBSan (211 tests each),
60 Metal API validation tests, 153 portable tests, 225 Python checks and all 195
unchanged coating/packet image gates. Both full-resolution day/night renders are
complete and independently audited. These checks remain distinct from the failed
scene-level quality target; see the [sampling/convergence record](sampling-convergence-validation.md).

The merged Pavilion implementation completed both unchanged 1600×850 entry points:
day at 256 spp and night at 512 spp, with verified PNG, raw FLOAT EXR and
checkpoints. At that revision, local Debug, Release and ASan/UBSan passed 204 tests each; Metal API
validation passed all 58 native tests. The packet comparison passed 150 gates,
and 89 legacy reference images plus demo accumulation were exact. See the
[Pavilion record](pavilion-validation.md) for artifact hashes, compiler checks,
checkpoint provenance and the remaining coated-BDPT comparison limitation.

Historical implementation `ba1982b` passed Debug, Release and ASan/UBSan checks (186 each),
portable CTest (140), Python checks (186), all 270 pinned PBRT comparison gates,
48 Metal tests with API validation, and exact legacy output comparisons for
81 scenes plus the demo.
See the [sampling record](sampling-defaults-validation.md) for the toolchain,
GPU validation, tolerances and artifact hashes.
The [mesh record](mesh-shape-validation.md) retains historical evidence for
`ff81056`; earlier area-power, camera, lighting, material, media, texture, importer,
HDR and checkpoint records likewise identify their own validation revisions.
