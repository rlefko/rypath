# Project status

## Current milestone

Perspective depth of field merged in [PR #45](https://github.com/rlefko/rypath/pull/45)
at `95ac4fc`. Configurable box-filter radii merged in
[PR #46](https://github.com/rlefko/rypath/pull/46) at `cd6a55c`, retaining default
box and triangle arithmetic with matching lens rays and texture differentials.

Diffuse area lights now accept positive power for each generated triangle or
bilinear patch. Geometry-dependent factors preserve shared spectral tables and
apply consistently to visible, surface and volume emission. Image means and
bilinear area follow pinned PBRT conventions and their documented limitations.
Identity schema 9 and renderer contract 11 reject older checkpoints. See the
[profile](pbrt-rendering.md) and [validation record](area-power-validation.md).

The next bounded acceptance task remains inventorying supported unmodified
upstream entry points and their licensed dependency closures. Long-run
convergence and measured M4 Max benchmarks remain on the [roadmap](roadmap.md).
Offline MVP completion and full pinned-corpus coverage remain unestablished.

## Validation

Area-power checks and exact artifacts are recorded in the
[validation record](area-power-validation.md), with revision-specific evidence.
The [box-filter record](box-filter-validation.md) retains its independent
geometric, lens, continuation and pinned PBRT results. Earlier lighting, media,
material, importer, texture, HDR and checkpoint records retain historical
evidence for their named revisions.
