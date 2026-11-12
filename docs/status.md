# Project status

## Current milestone

[Foundation PR #1](https://github.com/rlefko/rypath/pull/1) and
[Metal execution PR #2](https://github.com/rlefko/rypath/pull/2) were reviewed and
merged. The current PR adds native indexed triangle and instance acceleration,
reusable closest-hit and occlusion queries, and independent reference checks.
It awaits maintainer review.

The CLI remains unchanged. Geometry validation runs through CTest. There is no
scene ingestion, transport estimator, or image rendering yet. See the
[geometry contract](geometry.md) and [ordered PR plan](roadmap.md).

## Local validation

On November 12, 2026, native Debug, Release, and ASan/UBSan builds each passed
14 tests on the physical Apple M4 Max with no availability skips. Metal API
validation passed all four native backend tests. The environment was macOS 26.6,
Xcode 26.6 / Apple Clang 21, Metal compiler 32023.883, CMake 4.0.3, and Ninja 1.13.2.
The device reported architecture `applegpu_g16s` and compute ray-tracing support.

Each query type compared 281 rays (one single ray, 257 fixed-seed rays, and 23
analytic cases) and verified 746 guard elements. Maximum observed absolute errors
were `3.17891e-7` for t, `2.98023e-8` for a barycentric weight, and `3.57628e-7` for
world position, within the predeclared budgets. Separate exact-boundary probes
observed misses at both interval endpoints and hits on the isolated outer edge
and vertex. Those observations are not stronger cross-device inclusion promises.

The portable-only sanitizer build passed 12 tests. Both native static analyzer
passes, pinned formatting, portable LLVM 18 lint, and policy fixtures passed.
Independent review found no unresolved correctness issues. Its optimized
validator agreed with exact integer arithmetic on 53,300 affine determinants,
including 15,000 singular matrices. The committed cancellation fixtures also
detect a deliberately omitted FMA residual.

Temporary M4 Max shader probes removed each query's bounds check independently;
each produced the expected guard failure with no partial results, while the
other query still passed. Missing, invalid, and smoke-only libraries produced
stage-specific failures with no returned scene. These probes establish failure
detection, not rendering correctness or performance.

## Hosted checks and review

Hosted results and elapsed CI time are recorded in the PR against its final
revision. The macOS job compiles both Objective-C++ adapters and MSL with Xcode
16.4 and attempts native queries. Missing-device and missing-ray-tracing skips
are separately opted in and reported; compute tests still run without ray
tracing support. The physical M4 Max result above is mandatory and separate.

Linux runs portable tests in Debug, Release, and ASan/UBSan, plus compiled C++
CodeQL. CodeQL does not cover Objective-C++ or MSL. Main protection retains PRs,
current `Foundation Gate`, resolved discussions, CodeQL error/high-security
thresholds, and no force pushes or bypasses. Formal required approvals remain
zero so the maintainer can manually merge after review.

## Next bounded task

After review and merge: the first reproducible progressive image, with spectral
path state, a declared small transport model, complete sample batches, raw sample
accounting, shading-normal orientation, and independent image validation.
