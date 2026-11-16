# Project status

## Current milestone

Polygonal OBJ/MTL, STL and static glTF/GLB profiles merged in PRs #34–36;
spectral point and spot lighting merged in PRs #37–38, through `381512f`.
Their profiles retain explicit supported semantics and deferred features.

[PR #39](https://github.com/rlefko/rypath/pull/39) repairs bounded-fog null
crossings and finite visibility endpoints. The dependent distant-light change
adds up to 64 directional sources with spectral radiance, affine aiming and
PBRT-style scene-bound attenuation for surfaces and homogeneous volumes.
Geometry-free distant scenes remain unsupported. Renderer contract 6 and
identity schema 5 mark the new data and behavior; see the
[distant profile](pbrt-rendering.md#distant-lights).

Long-run convergence, licensed corpus acceptance and measured M4 Max benchmarks
remain on the [roadmap](roadmap.md). Offline MVP completion and full pinned-corpus
coverage remain unestablished.

## Validation

Distant implementation `0d92160` passes Debug, Release and ASan/UBSan (169/169
each), Metal-disabled LLVM 18 (130/130), all 41 Metal API tests without skips,
127 Python checks, static analysis and hooks. Nine pinned PBRT comparisons pass
all 135 region/channel gates; three shader mutations fail the intended tests,
and nine existing scenes retain byte-identical linear output. Both samplers
resume to exact Film state, and two 768×512, 512-spp samples complete. See the
[distant validation record](distant-light-validation.md) for independent review,
numerical limits and reproducible checks.

Fog repair `bc4dacc` passes Debug, Release and ASan/UBSan (167/167 each),
Metal-disabled LLVM 18 (129/129), all 40 Metal API tests without availability
skips, 119 Python checks, changed-unit static analysis and hooks. Both original
bounded scenes complete at 768×512, 512 spp, and both pinned PBRT volume cases
pass all 30 region/channel gates. See the [repair record](bounded-fog-validation.md).

Earlier [spot](spot-light-validation.md), [point](point-light-validation.md),
[glTF](gltf-validation.md), [STL](stl-validation.md), [OBJ](obj-validation.md),
surface, media, texture, HDR and checkpoint records retain historical evidence
for their recorded revisions.
