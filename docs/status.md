# Project status

## Current milestone

Loop subdivision and inline bilinear meshes merged in
[PR #48](https://github.com/rlefko/rypath/pull/48) at `b2eae1c`.

The current compatibility slice adds Halton, ZSobol, tabulated Gaussian filtering
and pinned PBRT defaults for omitted descriptors. Explicit unsupported features
still receive diagnostics. Stable absolute sample identities, owned filter
distributions and fixed ZSobol budgets support progressive execution and resume.
See the [render profile](pbrt-rendering.md) and
[sampling validation](sampling-defaults-validation.md).
Identity/renderer/sampling contracts are 10/12/3; format and accumulation remain 1.

Selected owned and CC0-derived fixtures exercise sampling, filtering, lenses and
homogeneous media. They do not establish additional unmodified upstream scene
coverage. Analytic geometry, additional film/camera/filter/sampler models, advanced
materials and heterogeneous media remain broader conformance work. A complete
pinned inventory, licensed dependency closures, long-run convergence and measured
M4 Max benchmarks remain acceptance work on the [roadmap](roadmap.md).
Offline MVP completion and full pinned-corpus coverage remain unestablished.

## Validation

Current sampling/default checks and their exact implementation revision will be
recorded in the [sampling record](sampling-defaults-validation.md) before delivery.
The [mesh record](mesh-shape-validation.md) retains historical evidence for
`ff81056`; earlier area-power, camera, lighting, material, media, texture, importer,
HDR and checkpoint records likewise identify their own validation revisions.
