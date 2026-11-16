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

The [mesh record](mesh-shape-validation.md) defines the fixed geometry/image
protocol and records revision-specific evidence separately from earlier checks.
Area-power, box-filter, lighting, media, material, importer, texture, HDR and
checkpoint records retain historical evidence for their named revisions.
