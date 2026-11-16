# Project status

## Current milestone

Polygonal OBJ/MTL, STL and static glTF/GLB profiles merged in PRs #34–36;
spectral point and spot lighting merged in PRs #37–38, through `381512f`.
Their profiles retain explicit supported semantics and deferred features.

[PR #39](https://github.com/rlefko/rypath/pull/39) repairs bounded-fog null
crossings and finite visibility endpoints. Distant lights merged in
[PR #41](https://github.com/rlefko/rypath/pull/41), through `1dc7b1f`, with
PBRT-style scene-bound attenuation and explicit geometry-free limitations.

[PR #42](https://github.com/rlefko/rypath/pull/42) adds spectral goniometric sources with
native scalar angular maps, nearest equal-area lookup and photometric power.
Surface, bounded-volume and geometry-free volume paths share the positional
estimator. The profile explicitly restricts transforms and scalar numeric inputs;
renderer contract 7 and identity schema 6 mark the new behavior. See the
[goniometric profile](pbrt-rendering.md#goniometric-lights) and
[validation record](goniometric-light-validation.md). Projection lights are the
next bounded lighting extension.

Long-run convergence, licensed corpus acceptance and measured M4 Max benchmarks
remain on the [roadmap](roadmap.md). Offline MVP completion and full pinned-corpus
coverage remain unestablished.

## Validation

Goniometric implementation through `f2365a7` passes Debug, Release and ASan/UBSan
(172/172 each), plus all 42 Metal API tests without skips. Portable LLVM 18 and
Python checks pass 132/132 and 135/135 on
`ef31a48`, before the Metal-only execution repair. All 135 pinned-PBRT gates pass;
four mutations fail the intended tests, and ten earlier scenes retain
byte-identical raw output. Both samplers resume to exact Film state. Surface and
bounded-fog samples complete at 768×512 with 512 and 4,096 spp. See the
[validation record](goniometric-light-validation.md) for numerical limits,
independent review and reproducible evidence.

Earlier [distant](distant-light-validation.md), [bounded fog](bounded-fog-validation.md),
[spot](spot-light-validation.md), [point](point-light-validation.md),
[glTF](gltf-validation.md), [STL](stl-validation.md), [OBJ](obj-validation.md),
surface, media, texture, HDR and checkpoint records retain historical evidence
for their recorded revisions.
