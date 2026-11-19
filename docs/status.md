# Project status

## Current milestone

Pavilion scene support merged in
[PR #50](https://github.com/rlefko/rypath/pull/50) at `dd8682d5`.

The current compatibility slice renders the pinned Barcelona Pavilion day and
night scenes: native sphere/disk/open cylinder, four-wavelength vacuum transport,
BDPT camera/light connections and splats, finite sensor-space film clamping,
raw EXR export, and an explicit bounded large-scene profile. Checkpoint
format/identity/renderer/sampling/accumulation contracts on this follow-up branch
are 3/12/17/7/2.
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
near-edge native triangle hits in the day replay. Their corrections have focused
native validation; final scene-level measurements remain separate from the
historical results below.
Unsupported media and geometry receive
diagnostics rather than substitutions.

This remains selected-scene coverage. Curves, motion, heterogeneous media,
additional camera/film models and broader material semantics remain on the
[roadmap](roadmap.md). Offline MVP acceptance and full pinned-corpus coverage
remain unestablished.

## Validation

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
