# Project status

## Current milestone

Pavilion scene support merged in
[PR #50](https://github.com/rlefko/rypath/pull/50) at `dd8682d5`.

The current compatibility slice renders the pinned Barcelona Pavilion day and
night scenes: native sphere/disk/open cylinder, four-wavelength vacuum transport,
BDPT camera/light connections and splats, finite sensor-space film clamping,
raw EXR export, and an explicit bounded large-scene profile. Checkpoint
format/identity/renderer/sampling/accumulation contracts on this follow-up branch
are 3/12/14/5/2.
See the [Pavilion record](pavilion-validation.md) for model limitations, acquisition,
acceptance targets and historical validation. The follow-up adds native environment
CDFs, combined light selection, directional shape/emission proposals and pinned
coating RNG domains; [sampling/convergence validation](sampling-convergence-validation.md)
tracks its current evidence and pending acceptance. Unsupported media and geometry receive
diagnostics rather than substitutions.

This remains selected-scene coverage. Curves, motion, heterogeneous media,
additional camera/film models and broader material semantics remain on the
[roadmap](roadmap.md). Offline MVP acceptance and full pinned-corpus coverage
remain unestablished.

## Validation

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
