# Project status

## Current milestone

Sampling and PBRT defaults merged in
[PR #49](https://github.com/rlefko/rypath/pull/49) at `46b20f4`.

The current compatibility slice targets the pinned Barcelona Pavilion day and
night scenes: native sphere/disk/open cylinder, four-wavelength vacuum transport,
BDPT camera/light connections and splats, finite sensor-space film clamping,
raw EXR export, and an explicit bounded large-scene profile. Checkpoint
format/identity/renderer/sampling/accumulation contracts are 2/11/13/4/2.
See the [Pavilion record](pavilion-validation.md) for model limitations, acquisition,
acceptance targets and current validation. Unsupported media and geometry receive
diagnostics rather than substitutions.

This remains selected-scene coverage. Curves, motion, heterogeneous media,
additional camera/film models and broader material semantics remain on the
[roadmap](roadmap.md). Offline MVP acceptance and full pinned-corpus coverage
remain unestablished.

## Validation

Historical implementation `ba1982b` passed Debug, Release and ASan/UBSan checks (186 each),
portable CTest (140), Python checks (186), all 270 pinned PBRT comparison gates,
48 Metal tests with API validation, and exact legacy output comparisons for
81 scenes plus the demo.
See the [sampling record](sampling-defaults-validation.md) for the toolchain,
GPU validation, tolerances and artifact hashes.
The [mesh record](mesh-shape-validation.md) retains historical evidence for
`ff81056`; earlier area-power, camera, lighting, material, media, texture, importer,
HDR and checkpoint records likewise identify their own validation revisions.
