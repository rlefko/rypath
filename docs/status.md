# Project status

## Current milestone

Polygonal OBJ/MTL, STL and static glTF/GLB profiles merged in PRs #34–36.
Spectral point, spot and distant lighting and the bounded-fog repair merged in
PRs #37–41. Goniometric lighting merged in [PR #42](https://github.com/rlefko/rypath/pull/42),
through `d992a48`.

[PR #43](https://github.com/rlefko/rypath/pull/43) adds spectral RGB projection
lights for surfaces and bounded/geometry-free homogeneous media. Native nearest
perspective images preserve affine scale and shear, with explicit photometric
power and numerical limits. Renderer contract 8 and identity schema 7 reject
incompatible checkpoints. See the [profile](pbrt-rendering.md#projection-lights)
and [validation record](projection-light-validation.md).

An exact-tangent triangle emitter hit now uses the same zero-light-PDF rule as
bilinear patches, repairing a high-sample failure at a floor/light junction.
Renderer contract 9 rejects prior checkpoints; identity remains 7. See the
[tangent emission record](tangent-emission-validation.md) for the reduced
regression, original reproduction and prism-dispersion demonstration.

The next bounded acceptance task is to inventory supported unmodified upstream
entry points and their licensed dependency closures. Long-run convergence and
measured M4 Max benchmarks remain on the [roadmap](roadmap.md). Offline MVP
completion and full pinned-corpus coverage remain unestablished.

## Validation

Projection implementation `c3cd663` and test-harness correction `9ef0049` pass
Debug, Release and ASan/UBSan (174/174 each). Strict Metal API validation passes
43/43 without skips. Portable LLVM 18 passes 133/133; Python passes 144/144;
focused static analysis and repository hooks pass. All 165 pinned-PBRT gates
pass, five intentional mutations are detected, and eleven earlier scenes retain
byte-identical raw output. Both samplers resume to exact Film state.

Surface and bounded-fog samples complete at 768x512 and 512 spp; fog also completes
at 4,096 spp. The validation record identifies source revisions, binary hashes,
independent review and reproducible evidence. These images do not establish
convergence or performance results. Earlier lighting, media, material, importer,
texture, HDR and checkpoint records retain historical evidence for their named
revisions.
