# Project status

## Current milestone

Perspective depth of field merged in [PR #45](https://github.com/rlefko/rypath/pull/45)
at `95ac4fc`, following the importer, spectral lighting, media and material work.

Box filters now support independently authored positive radii. Nondefault boxes
sample a normalized pixel-centered rectangle, including outside-image support,
with matching lens rays and texture differentials. The default half-pixel box
and existing triangle paths retain their arithmetic. See the
[profile](pbrt-rendering.md) and [validation record](box-filter-validation.md).
No new sampler dimensions or checkpoint contracts are introduced.

The next bounded acceptance task remains inventorying supported unmodified
upstream entry points and their licensed dependency closures. Long-run
convergence and measured M4 Max benchmarks remain on the [roadmap](roadmap.md).
Offline MVP completion and full pinned-corpus coverage remain unestablished.

## Validation

Current box-filter checks and their exact artifacts are recorded in the
[validation record](box-filter-validation.md). They include independent geometric
coverage, lens differentials, serialized continuation and pinned PBRT comparisons.
Earlier lighting, media, material, importer, texture, HDR and checkpoint records
retain historical evidence for their named revisions.
