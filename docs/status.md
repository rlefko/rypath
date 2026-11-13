# Project status

## Current milestone

[Foundation PR #1](https://github.com/rlefko/rypath/pull/1),
[Metal execution PR #2](https://github.com/rlefko/rypath/pull/2),
[native geometry PR #3](https://github.com/rlefko/rypath/pull/3),
[progressive image PR #4](https://github.com/rlefko/rypath/pull/4),
[PBRT preflight PR #5](https://github.com/rlefko/rypath/pull/5),
[PBRT spectral rendering PR #6](https://github.com/rlefko/rypath/pull/6),
[sRGB reconstruction PR #7](https://github.com/rlefko/rypath/pull/7), and
[triangle area-light PR #8](https://github.com/rlefko/rypath/pull/8) were reviewed
and merged. The current PR adds normalized triangle pixel filtering.

`--render-pbrt SCENE --output IMAGE.png` translates supported sampled spectra and
linear sRGB colors, diffuse triangles/instances, a pinhole camera, constant environment lighting,
directly placed triangle emitters, and box or triangle pixel filters
into native Metal rendering. It honors authored finite depth, retains calibrated
raw XYZ, reports complete sample batches, and publishes a final PNG. The
[rendering profile](pbrt-rendering.md) documents exact semantics and limits.
Portable `--check-pbrt` remains structural analysis and never renders.

The demo retains its original relative spectral model and compensated roulette.
Wider color spaces, positive-power normalization, textured/instanced emitters,
external meshes/textures, general upstream-scene
rendering, OpenEXR, checkpointing, and the interactive app remain future work.
The pinned Cornell fixture is still an explicit unsupported case; a separately
labeled CC0 adaptation exercises the supported area-light profile.

## Validation and review

The [triangle-filter validation record](pbrt-filter-validation.md) extends the
[PBRT render validation record](pbrt-render-validation.md) with portable
translation checks, M4 Max regressions, pinned upstream CPU comparisons, and
numerical limitations. Hosted checks, CodeQL findings, and measured CI duration
are attached to the PR against its final revision.

Linux runs portable Debug, Release, and sanitizer tests plus compiled C++
CodeQL. macOS uses Xcode 16.4 for Objective-C++/MSL compilation, native static
analysis, and execution attempts. Explicit missing-capability skips are separate
from required strict M4 Max passes. CodeQL does not cover Objective-C++ or MSL.

Main protection retains PRs, current Foundation Gate, resolved discussions,
CodeQL error/high-security thresholds, and no force pushes or bypasses. Formal
required approvals remain zero for maintainer manual review and merge.

## Next bounded task

After review and merge, plan the next bounded compatibility slice toward
faithfully rendered unmodified upstream scenes. Sobol sampling and triangle
normal/UV attributes still block the unmodified Cornell fixture. The ordered
[roadmap](roadmap.md) keeps external assets, broader materials, and offline
completion as separate review boundaries.
