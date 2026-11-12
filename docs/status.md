# Project status

## Current milestone

[Foundation PR #1](https://github.com/rlefko/rypath/pull/1),
[Metal execution PR #2](https://github.com/rlefko/rypath/pull/2),
[native geometry PR #3](https://github.com/rlefko/rypath/pull/3), and
[progressive image PR #4](https://github.com/rlefko/rypath/pull/4) were reviewed
and merged. The current PR adds portable PBRT v4 structural preflight and awaits
maintainer review.

`--check-pbrt SCENE` reports syntax, checked scene structure, dependencies, and
unsupported features without rendering or creating outputs. The
[compatibility profile](pbrt-profile.md) defines exactly what is checked.
A small unmodified, checksum-pinned CC0 Cornell fixture exercises selected
external syntax and inventory; its unsupported features remain explicit.

`--render-demo` still renders original matte geometry using native Metal
acceleration, scalar wavelength sampling, environment NEE/MIS, and compensated
roulette. Complete batches commit raw XYZ and sample counts; terminal progress
and a final sRGB PNG expose the result. There is no external-scene rendering,
general material system, OpenEXR, checkpointing, or interactive application yet.
See the [rendering contract](rendering.md) and [ordered PR plan](roadmap.md).

## Validation and review

The [preflight validation record](pbrt-validation.md) records portable checks,
resource and numerical boundaries, existing M4 Max rendering regressions,
static analysis, and limitations for this PR. Hosted results, CodeQL findings,
and measured CI duration are attached to the PR against its final revision.

Linux runs portable Debug, Release, and sanitizer tests plus compiled C++
CodeQL. macOS uses Xcode 16.4 to compile Objective-C++ and MSL, analyze native
sources, and attempt execution. Explicit missing-device and missing-ray-tracing
skips are separate from the mandatory local M4 Max result; errors after acquiring
supported capability always fail. CodeQL does not cover Objective-C++ or MSL.

Main protection retains PRs, current `Foundation Gate`, resolved discussions,
CodeQL error/high-security thresholds, and no force pushes or bypasses. Formal
required approvals remain zero so the maintainer can manually merge after review.

## Next bounded task

After review and merge, plan the first external PBRT rendering translation with
explicit camera, spectral/color, material, lighting, and asset contracts.
Preflight success alone does not establish rendering compatibility. JSON reports
and broader corpus coverage remain follow-up work.
