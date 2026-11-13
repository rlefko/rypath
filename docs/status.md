# Project status

## Current milestone

[Foundation PR #1](https://github.com/rlefko/rypath/pull/1),
[Metal execution PR #2](https://github.com/rlefko/rypath/pull/2),
[native geometry PR #3](https://github.com/rlefko/rypath/pull/3),
[progressive image PR #4](https://github.com/rlefko/rypath/pull/4),
[PBRT preflight PR #5](https://github.com/rlefko/rypath/pull/5),
[PBRT spectral rendering PR #6](https://github.com/rlefko/rypath/pull/6),
[sRGB reconstruction PR #7](https://github.com/rlefko/rypath/pull/7),
[triangle area-light PR #8](https://github.com/rlefko/rypath/pull/8),
[triangle-filter PR #9](https://github.com/rlefko/rypath/pull/9),
[triangle-attribute PR #10](https://github.com/rlefko/rypath/pull/10), and
[Sobol PR #11](https://github.com/rlefko/rypath/pull/11) were reviewed and merged.
The renderer supports all four PBRT Sobol randomizations and renders the
unchanged pinned Cornell fixture at its authored settings. Current work audits
foundation verification and project agent guidance; it adds no rendering features.

`--render-pbrt SCENE --output IMAGE.png` translates supported sampled spectra and
linear sRGB colors, diffuse triangles/instances with interpolated normals and retained UVs,
a pinhole camera, constant environment lighting,
directly placed triangle emitters, box or triangle pixel filters, and independent
or Sobol sampling
into native Metal rendering. It honors authored finite depth, retains calibrated
raw XYZ, reports complete sample batches, and publishes a final PNG. The
[rendering profile](pbrt-rendering.md) documents exact semantics and limits.
Portable `--check-pbrt` remains structural analysis and never renders.

The demo retains its original relative spectral model and compensated roulette.
Wider color spaces, positive-power normalization, textured/instanced emitters,
external meshes/textures, general upstream-scene
rendering, OpenEXR, checkpointing, and the interactive app remain future work.
The pinned Cornell fixture completes 1024×1024 × 64 spp with depth 65. This is
one supported unmodified external entry point; broad corpus support is not established.

## Validation and review

The foundation audit adds a 37-case CLI availability classification harness and
archive/commit-hook regressions. On Apple M4 Max (`applegpu_g16s`), macOS 26.6
(25G5065a), and Xcode 26.6 (17F113), all 58 tests passed in each of Debug, Release,
and ASan/UBSan with both availability allowances disabled. Metal API validation
passed all 10 selected tests. All 24 Python policy/comparison tests passed;
formatting, skill validation, agent TOML parsing, and four independent guidance
scenarios also passed. These are foundation regression results; the renderer and
its numerical thresholds are unchanged.

The [Sobol validation record](pbrt-sobol-validation.md) records portable
conformance, exact CPU/MSL sampling checks, deterministic quadrature, M4 Max
57/57 tests in each build preset, and 222/222 pinned CPU PBRT comparison gates.
Hosted checks, CodeQL findings, and measured CI duration
are attached to the PR against its final revision.

Linux runs portable Debug, Release, and sanitizer tests plus compiled C++
CodeQL. macOS uses Xcode 16.4 for Objective-C++/MSL compilation, native static
analysis, and execution attempts. Explicit missing-capability skips are separate
from required strict M4 Max passes. CodeQL does not cover Objective-C++ or MSL.

Main protection retains PRs, current Foundation Gate, resolved discussions,
CodeQL error/high-security thresholds, and no force pushes or bypasses. Formal
required approvals remain zero for maintainer manual review and merge.

## Next bounded task

After the foundation audit, plan external PLY mesh ingestion with dependency
diagnostics and a small pinned asset-backed PBRT entry point. The ordered
[roadmap](roadmap.md) keeps external assets, broader materials, and offline
completion as separate review boundaries.
