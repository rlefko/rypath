# Project status

## Current milestone

Area-light power merged in [PR #47](https://github.com/rlefko/rypath/pull/47)
at `42c3079`, following perspective depth of field and configurable box radii.

The current geometry slice adds checked Loop subdivision and inline bilinear
meshes. Loop refinement preserves limit positions and raw normals; inline quads
reuse the existing exact patch backend. Both use existing materials, alpha,
static instances, media and direct area lights, including per-generated-primitive
power. Independent topology/scratch/work bounds reject invalid or excessive
subdivision. See the [profile](pbrt-rendering.md) and
[validation record](mesh-shape-validation.md).
Identity schema 9, renderer contract 11 and sampling contract 2 remain unchanged.

Selected licensed Loop excerpts extend asset coverage, without establishing
support for the original bathroom scene. Halton and default-Gaussian/descriptor
handling are recurring blockers in inspected upstream entry points; a complete
pinned inventory and licensed dependency closures remain acceptance work. Long-run
convergence and measured M4 Max benchmarks remain on the [roadmap](roadmap.md).
Offline MVP completion and full pinned-corpus coverage remain unestablished.

## Validation

At implementation revision `ff81056`, the [mesh record](mesh-shape-validation.md)
records 36 passing geometry-oracle cases, 24 image cases / 360 statistical gates,
69 byte-exact legacy scenes, complete Release and ASan/UBSan suites (180 each),
portable Clang 18 tests (137), and strict M4 Max Metal API validation (45).
It separates current evidence from earlier checks.
Area-power, box-filter, lighting, media, material, importer, texture, HDR and
checkpoint records retain historical evidence for their named revisions.
