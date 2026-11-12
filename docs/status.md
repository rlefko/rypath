# Project status

## Current milestone

Milestone 1 is complete: [foundation PR #1](https://github.com/rlefko/rypath/pull/1)
was reviewed and merged. The first slice of milestone 2 adds Metal device
diagnostics, offline shader compilation, and a checked compute dispatch. It is
awaiting PR validation and maintainer review.

The CLI now has `--metal-info` and `--metal-smoke-test`. There is no scene
ingestion, acceleration structure, transport estimator, or image rendering yet.

## Validation

On November 12, 2026, strict Debug, Release, and ASan/UBSan builds each passed all
12 tests on the physical Apple M4 Max with no availability skips. The environment
was macOS 26.6, Xcode 26.6 / Apple Clang 21, Metal compiler 32023.883, CMake 4.0.3,
and Ninja 1.13.2. The device reported architecture `applegpu_g16s`, unified memory,
and both ray-tracing API flags. Hardware ray-tracing execution remains untested.

The compute check verified two dispatches, 258 logical values, and 512 guard
elements. All three backend tests passed with Metal API validation enabled.
Temporary corrupted shaders were rejected for a changed output at index 0 and a
guard write at index 1. These probes establish that the checks detect both error
classes; they are not transport or performance benchmarks.

The macOS portable-only sanitizer build passed 10 tests. A sandbox with no
visible device produced four explicit skips only when allowed; strict mode
failed those tests. Missing/invalid library cases, canonical asset lookup via a
symlink from another directory, static analysis, and independent review passed.
Deleting the generated library rebuilt it without relinking the executable.

Hosted Linux, Xcode 16.4, and CodeQL results will be recorded after the PR runs.
The macOS deployment minimum is 14; local runtime evidence is from macOS 26.6.

## Review workflow

`main` retains required PRs, current `Foundation Gate`, resolved discussions,
CodeQL error/high-security thresholds, and no force pushes or bypasses. Formal
required approvals are zero so the maintainer can manually merge after review.
No workstation runner automatically executes untrusted external PR code.

## Next bounded task

After review and merge of the Metal execution PR: native triangle/instance
acceleration structures and independent intersection/occlusion checks. See the
[ordered PR plan](roadmap.md).
