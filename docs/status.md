# Project status

## Current milestone

[Foundation PR #1](https://github.com/rlefko/rypath/pull/1),
[Metal execution PR #2](https://github.com/rlefko/rypath/pull/2), and
[native geometry PR #3](https://github.com/rlefko/rypath/pull/3) were reviewed and
merged. The current PR adds the first progressive spectral image and awaits
maintainer review.

`--render-demo` renders original matte geometry using native Metal acceleration,
scalar wavelength sampling, environment NEE/MIS, and compensated roulette.
Complete batches commit compensated raw XYZ and sample counts; terminal progress
and a final sRGB PNG expose the result. There is no scene ingestion, general
material system, OpenEXR, checkpointing, or interactive application yet. See the
[rendering contract](rendering.md) and [ordered PR plan](roadmap.md).

## Local validation

On November 12, 2026, physical M4 Max Debug, Release, and ASan/UBSan builds each
passed **27/27 tests**, without availability skips. Metal API validation passed
all **5/5 native backend tests**. The environment was macOS 26.6 (25G5065a),
Xcode 26.6 / Apple Clang 21, Metal 32023.883, CMake 4.0.3, and Ninja 1.13.2.
The system-default device reported architecture `applegpu_g16s`, registry ID
4294969039, and compute ray-tracing support.

Background and diffuse-floor cases each tested 32,768 paths against independent
spectral/directional quadrature. Multibounce comparison used 16 independent
replicates per backend, each at 4x4 pixels and 128 spp:

| Case | GPU mean XYZ | Analytic or CPU mean XYZ | Largest difference in standard errors |
| --- | --- | --- | --- |
| Background | 0.991927, 0.997218, 1.002200 | 1.000080, 1.000000, 1.000330 | 1.06 |
| Diffuse floor | 0.525930, 0.498626, 0.197796 | 0.529864, 0.499726, 0.198259 | 0.79 |
| Multibounce | 0.495216, 0.486943, 0.467542 | 0.495424, 0.497439, 0.454906 | 1.51 |

All passed the predeclared six-/five-standard-error gates and numerical floors.
Maximum path scattering counts were 19 on GPU and 18 in the independent CPU
reference. Across 16 fixed seeds, the pooled 64-to-16-spp RMSE ratio was
**0.487209**, within the predeclared 0.35–0.70 range.

Raw film matched exactly across batch partitions, pixel-tile ordering, and
threadgroup widths, including 17x13 images and the driver's 4,099-pixel/65,536-path
boundaries. Tests covered camera orientation, native-to-source instance mapping,
32-bit sample-index rollover, ownership after input destruction, tilted and
mirrored normals, a blocker 0.002 units away, and failed-batch preservation.
The planar self-intersection regression now requires every isolated-floor path
to scatter exactly once; the numerical offset remains a bounded empirical Metal
adaptation, not a universal error guarantee.

Four deterministic transport paths checked both MIS arms, retained NEE on
roulette termination, survivor compensation, and a raw value above one.
Independent temporary shader mutations that removed a bounds check, discarded
NEE at roulette termination, or omitted survival compensation all failed their
intended checks with no partial successful result. PNG checks decoded dimensions,
sRGB tagging and expected bytes, preserved raw film, and exercised publication
failure without replacing the previous output.

The Metal-disabled sanitizer build passed **24/24 tests**. All five Objective-C++
analyzer passes, portable LLVM 18 lint, pinned formatting/hooks, and seven policy
fixtures passed. Shader-output deletion rebuilt AIR/metallib without a C++
relink. The demo PNG was rendered and visually inspected; these results make no
performance claim.

## Hosted checks and review

Hosted results, CodeQL findings, and measured CI duration are recorded in the PR
against its final revision. Linux runs portable Debug, Release, and sanitizer
tests plus compiled C++ CodeQL. macOS uses Xcode 16.4 to compile Objective-C++ and
MSL, analyze native sources, and attempt execution. Explicit missing-device and
missing-ray-tracing skips are separate from the mandatory local M4 Max result;
errors after acquiring supported capability always fail.

Main protection retains PRs, current `Foundation Gate`, resolved discussions,
CodeQL error/high-security thresholds, and no force pushes or bypasses. Formal
required approvals remain zero so the maintainer can manually merge after review.
CodeQL does not cover Objective-C++ or MSL.

## Next bounded task

After review and merge: evaluate and establish the first PBRT v4 ingestion
profile, with dependency/license review, canonical scene translation,
source-located preflight diagnostics, and explicit compatibility coverage.
