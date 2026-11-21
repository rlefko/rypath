# Project status

## Current milestone

Pavilion scene support merged in [PR #50](https://github.com/rlefko/rypath/pull/50)
at `dd8682d5`. The active follow-up is
[PR #51](https://github.com/rlefko/rypath/pull/51), which remains a draft while
performance and raw-image quality regressions are investigated. It must not be
interpreted as completed PBRT parity.

The supported slice renders the pinned Barcelona Pavilion day and night scenes
with sphere/disk/open-cylinder geometry, four-wavelength vacuum transport,
BDPT connections and splats, raw EXR export and separately accumulated authored
film clamping. The follow-up adds environment CDFs, combined light selection,
directional shape/emission proposals and coating RNG domains. See the
[Pavilion record](pavilion-validation.md) for the physical model, scene acquisition
and explicit overrides, and the [sampling record](sampling-convergence-validation.md)
for the original study.

Current checkpoint format/identity/renderer/sampling/accumulation contracts are
**3/12/20/7/2**. Renderer 19 repairs full-sphere pole clipping. Renderer 20 adds
validated native-triangle hints to eligible packet scenes, preserving procedural
validation and stable closest-hit ordering. The new packet resource ABI and
potentially recovered true intersections invalidate older checkpoints. See
[geometry](geometry.md) and [checkpointing](checkpointing.md).

## Findings and remaining gaps

The [transport regression record](transport-regression-validation.md) contains
the detailed evidence and immutable artifact hashes:

- Repeated Halton light paths across 128-pixel tiles are repaired. A missing
  importance shading-normal correction is also repaired; it caused the traced
  night outlier. Near-edge native triangle inconsistencies required compensated
  validation, broader candidate coverage and numerical boundary repairs.
- The frozen renderer-18 quality study still fails: **0/48** simultaneous
  variance/tail bounds and **26/36** raw mean safeguards pass. Day passes all
  18 mean checks; night passes eight. Fresh 1600×850 day-256 and night-512
  images are complete and independently checked, but do not establish parity.
- A separate 32-frame modified-PBRT diagnostic isolates much of the night
  brightness difference. Applying the same importance correction brings all six
  glass/floor RGB comparisons with rypath BDPT within the inherited rule.
  Floor red and green still differ from PBRT path tracing. Neither reference
  convergence nor variance parity follows from this result.
- An exact day replay identifies a rare refracted-environment path, rather than
  a display/filter artifact. A selected-pixel, 32-seed sampling diagnostic loses
  the observed outlier when either the hemisphere mapping or sampler changes.
  Selection and finite-sample limits prevent adopting a sampling change from
  this evidence alone.
- Integrated arithmetic and visibility optimizations reduce repeated GPU time
  by 37.0% by day and 45.1% by night against frozen renderer 18. A separate
  native-triangle prototype reduces it a further 20.3% and 24.3% in its measured
  comparison, with exact timing-image controls and about 1.13 GB of additional
  acceleration storage. Small-scene controls pass but show no established
  small-scene speedup. These are matched-workload results, not time to quality.
  A separate repeated arithmetic comparison reduces GPU time by 9.13%/8.86%
  and whole-process time by 4.66%/5.43%, with exact images. These component
  measurements are distinct from the combined production comparison below.

The batched day diagnostic reproduces all 128 earlier cases exactly. Its
4,096-spp extension is declined before extended outcomes: the measured forecast
is 676.126 seconds, above the unchanged 600-second operational limit. This adds
execution evidence, not additional tail observations.

Fresh renderer-20 day/night controls at 320×170 and 64 spp reproduce every
raw and authored-clamped FLOAT component and full Film byte of renderer 19.
Independent auditing also reproduces both standard-sRGB previews.
A direct 16-run production comparison now measures the combined changes. At
320×170 and 64 spp, three measured pairs after warm-up reduce whole-process
time by **12.7% day / 17.5% night** versus published renderer 19. Rendering and
accumulation take **26.3% / 29.7%** less time; these are host phases, not GPU
timestamps. All sixteen Film and raw/clamped FLOAT controls are exact. About
43 seconds remains outside the named phases. This is a fixed-sample workload
comparison, not time to a shared quality target.

The tested integration is the current review slice. Further work must isolate
the remaining process cost and scene-level discrepancies.
The remaining slowdown, heavy tails and BDPT/PT floor discrepancy are open.

## Validation boundaries

Current renderer 20 passes all 222 ASan/UBSan tests and 159 portable tests.
Release evidence combines 217 non-CLI checks with Metal API validation and five
CLI checks under their default environment. The initial full API-enabled run
fails those five CLI checks solely because its injected banner violates their
unchanged empty-stderr assertions; the failed run is retained. The native tests
include five affine transforms, complete seed-distance preservation and integer
rounding oracles. Both directions of the renderer-19/20 host-library mismatch
are rejected; the Python checkpoint decoder passes 12 tests.

Focused Clang-Tidy and static analysis of both changed production Metal adapters
and the native probe pass. Clang-Tidy first catches implicit integer widening in
a test's expected byte count; explicit operand types fix it, and the affected
test passes again in all four builds. CI then catches omitted transform
initializers in the new budget fixture under LLVM 18. Explicit identity
initializers preserve the fixture; matching LLVM 18 reproduces the error before
the fix and passes afterward, and the affected test passes in all four builds.
Publication `f87732b` passes all 222 Debug pre-push tests and all six jobs in
[CI run 44864412913](https://github.com/rlefko/rypath/actions/runs/44864412913).
Later publication checks are reported with their revision in PR #51.

Published renderer-19 revision `89e04a6` passes the full 220-test Debug pre-push
suite and all six jobs in
[CI run 44846373287](https://github.com/rlefko/rypath/actions/runs/44846373287).
Its sphere repair passes 17 focused analytic/checkpoint tests in each supported
preset with Metal API validation; the old shader first fails the new pole case.

Earlier execution changes pass 220 tests in each of Debug, Release and ASan/UBSan,
158 portable Clang checks, 64 native Debug checks with API validation, and focused
static analysis. All six jobs in
[CI run 44830941172](https://github.com/rlefko/rypath/actions/runs/44830941172)
pass at publication revision `b0b7bbb`. These checks and the older renderer-18
full-image audits are historical evidence, not checks of current renderer 20.
The [transport](transport-regression-validation.md),
[sampling](sampling-convergence-validation.md) and
[Pavilion](pavilion-validation.md) records retain prior failures and validation
at their respective revisions.

This remains selected-scene coverage. Unsupported media and geometry receive
diagnostics rather than substitutions. Curves, motion, heterogeneous media,
additional cameras/films, broader material semantics and full mesh-format
coverage remain on the [roadmap](roadmap.md). Offline MVP acceptance and full
pinned-corpus coverage remain unestablished.
