# PBRT preflight validation

This slice changes parsing, scene-state interpretation, and checked geometry
conversion. It changes no transport model, estimator, native traversal kernel,
or progressive accumulation behavior. Preflight never renders an external scene.

## Local results

On November 12, 2026, the final implementation passed:

| Check | Result |
| --- | --- |
| M4 Max Debug | 38/38 CTests, no availability skips |
| M4 Max Release | 38/38 CTests, no availability skips |
| M4 Max ASan/UBSan | 38/38 CTests, no availability skips |
| Metal-disabled ASan/UBSan | 35/35 CTests |
| Metal API validation | 5/5 native tests with `MTL_DEBUG_LAYER=1` |
| Apple Clang Static Analyzer | All five Objective-C++ sources |
| Portable LLVM 18 clang-tidy | Changed C++ sources and tests |
| Pinned hooks and repository policy | Formatting, whitespace, YAML, local-path guard and policy fixtures |

The system-default device was Apple M4 Max, architecture `applegpu_g16s`, registry
ID 4294969039, with unified memory and compute ray tracing. Toolchain: macOS 26.6
(25G5065a), Xcode 26.6 (17F113), Apple Clang 21.0.0, Metal 32023.883, CMake 4.0.3,
Ninja 1.13.2, and clang-format/clang-tidy 18.1.8. Builds retain macOS 14 as the
Metal deployment minimum. Existing GPU rendering and geometry comparisons use
their unchanged numerical thresholds.

## Coverage and independent review

The 11 added CTests comprise six portable preflight groups, four argument tests,
and one terminal-report/fixture test. They exercise lexical and numeric limits,
escapes, typed cardinality, precedence and unused parameters, source locations,
shared-state includes, dependencies, missing inputs, state lifetimes, camera and
matrix conventions, instance composition, spectral preservation, and float-domain
errors. Every configured processing limit has a small injected boundary test.
The unchanged Cornell fixture is checked against both pinned hashes, with eight
materials, eight meshes, 36 triangles, no input dependencies, and exactly 19
expected unsupported-feature diagnostics; see its
[manifest](../tests/fixtures/pbrt/README.md).

Independent reviews found and drove regression fixes for incomplete include
state, inherited reference ownership, repeated-name and dependency amplification,
overridden file parameters, semantic diagnostic limits, and singular transforms
hidden by rounded elimination or matrix composition. Permanent checks include
scoped deferred material references, shared long names, repeated asset paths,
realistic-camera replacement, clipped film bounds, and ambiguous continuation
following an unvisited compressed include.

The exact binary64 linear-determinant predicate passed 512 permanent independent
integer-reference cases with power-of-two scaling, plus explicit subnormal and
singular operands. Two separate reviewers compared it against exact rational
arithmetic on 40,000 and 10,000 matrices, including random finite bit patterns,
large/small exponents, duplicate/dependent rows, and perturbations. All agreed.
These tests validate zero classification; they do not establish a forward-error
bound for subsequent binary64 inversion or world-space intersection accuracy.

Separate probes exercised the actual default file, token, active-file, and
expansion boundaries, and the resolved-asset-path byte budget. The same
100,000-reference stress fixture now produces one external dependency rather
than repeated copies. All routine tests remain offline and require no PBRT binary.

## Limits and hosted evidence

No matching PBRT executable at the pinned revision was installed, so the optional
`--format` comparison was unavailable. The full upstream renderer and large scene
corpus were not downloaded or built. This is one selected preflight fixture,
not full PBRT scene conformance or a rendering comparison.

Final-revision hosted checks, measured CI duration, and CodeQL results are
recorded in the PR. Linux runs all portable tests in Debug, Release, and
ASan/UBSan plus compiled CodeQL. Hosted macOS uses Xcode 16.4 and retains explicit
availability skips only for observed missing GPU support; its availability is
reported separately from the mandatory local M4 Max runs. `Foundation Gate` and
main protection remain unchanged.
