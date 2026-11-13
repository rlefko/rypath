# Project status

## Current milestone

Foundations through [Sobol rendering PR #11](https://github.com/rlefko/rypath/pull/11)
and the [foundation audit PR #12](https://github.com/rlefko/rypath/pull/12) are merged.
The current PLY rendering change is ready for review: bounded ASCII/binary PLY
triangles and faithful bilinear quads, native patch intersection, interpolated
normals/retained UVs and constant curved emission. Its small pinned external asset
has a complete dependency/license manifest. It does not establish compatibility
with the full upstream Sportscar scene or general PBRT scene collection.

`--render-pbrt SCENE --output IMAGE.png` supports sampled spectra and linear sRGB,
diffuse triangle/patch instances, a pinhole camera, constant environment and
directly placed area lights, box/triangle filters, and independent or all four
Sobol sampling modes. It honors finite depth, retains calibrated raw XYZ and
complete sample batches, and publishes a final PNG. The [render profile](pbrt-rendering.md)
defines its numerical and semantic limits; portable `--check-pbrt` never renders.

The unchanged pinned Cornell fixture remains a supported external entry point.
Textures, wider color spaces, positive-power normalization, emitting object
members, other mesh formats, OpenEXR, checkpointing and the interactive app remain
future work. The [roadmap](roadmap.md) keeps these as separate review boundaries.

## Current validation

On Apple M4 Max (`applegpu_g16s`), macOS 26.6 (25G5065a), Xcode 26.6 (17F113),
all 64 CTests passed in Debug, Release and ASan/UBSan with both availability
allowances disabled. All 12 native tests passed Metal API and shader validation;
54 portable ASan/UBSan tests also passed. LLVM 18 clang-tidy passed 37 native and
40 Metal-disabled C++ units; Apple static analysis passed six Objective-C++ units.
All 25 Python policy/comparison tests passed.

The [PLY validation record](pbrt-ply-validation.md) records independent intersection,
shared-edge, quadrature, furnace, affine/offset and self-lighting evidence. All
267 pinned CPU PBRT comparison gates passed across 672 linear PFM outputs, with
unchanged seeds and thresholds. Independent review prompted the grazing/edge,
accepted-hit payload, transformation, numeric parsing and resource-budget
regressions. The three owned wrappers also completed inspected PNG renders.
Earlier milestones retain their historical evidence in their own validation records.

Hosted Linux/macOS checks and CodeQL are attached to the PR's final revision.
Hosted macOS permits only explicitly observed missing capabilities to skip;
those skips remain separate from strict M4 Max results. CodeQL covers portable
C++, not Objective-C++ or MSL. No performance improvement is claimed.

Main protection retains PRs, current Foundation Gate, resolved discussions,
CodeQL error/high-security thresholds and no force pushes or bypasses. Required
formal approvals remain zero for maintainer manual review and merge.

## Next bounded task

Plan a constant-texture translation slice before image acquisition/filtering and
textured emission. Keep broader import formats and material/transport additions
separately scoped. This PR remains unmerged pending maintainer review.
