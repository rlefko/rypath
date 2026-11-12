# Project status

## Current milestone

[Foundation PR #1](https://github.com/rlefko/rypath/pull/1),
[Metal execution PR #2](https://github.com/rlefko/rypath/pull/2),
[native geometry PR #3](https://github.com/rlefko/rypath/pull/3),
[progressive image PR #4](https://github.com/rlefko/rypath/pull/4), and
[PBRT preflight PR #5](https://github.com/rlefko/rypath/pull/5) were reviewed
and merged. The current PR adds a strict PBRT spectral rendering profile and
awaits maintainer review.

`--render-pbrt SCENE --output IMAGE.png` translates supported sampled spectra,
diffuse triangles/instances, a pinhole camera, and constant environment lighting
into native Metal rendering. It honors authored finite depth, retains calibrated
raw XYZ, reports complete sample batches, and publishes a final PNG. The
[rendering profile](pbrt-rendering.md) documents exact semantics and limits.
Portable `--check-pbrt` remains structural analysis and never renders.

The demo retains its original relative spectral model and compensated roulette.
RGB reconstruction, area lights, external meshes/textures, general upstream-scene
rendering, OpenEXR, checkpointing, and the interactive app remain future work.
The pinned Cornell fixture is still an explicit unsupported case.

## Validation and review

The [PBRT render validation record](pbrt-render-validation.md) records portable
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

After review and merge, plan the next spectral/color and lighting compatibility
slice toward faithfully rendered pinned upstream scenes. The ordered
[roadmap](roadmap.md) keeps external assets, broader materials, and offline
completion as separate review boundaries.
