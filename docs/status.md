# Project status

## Current milestone

[Extended PBRT surfaces (PR #31)](https://github.com/rlefko/rypath/pull/31) is
merged at `d8fe018`.

[Homogeneous spectral media (PR #32)](https://github.com/rlefko/rypath/pull/32)
adds explicit `volpath` with absorption,
scattering, absorption-coupled emission, Henyey–Greenstein phase functions,
scoped camera/instance interfaces, invisible boundaries and all 47 pinned PBRT
measured presets. Ordered shadow traversal includes medium transmittance;
raw Film accumulation remains separate. See the
[render profile](pbrt-rendering.md#homogeneous-media) and
[media validation record](medium-validation.md).

Heterogeneous media, arbitrary layers, subsurface/hair, fractional emitting alpha,
Ptex, wider color spaces, additional importers, checkpointing and the interactive
app remain on the [roadmap](roadmap.md). This does not establish offline MVP
completion or full pinned-corpus support. Durable checkpoint/resume with
cancellation remains a useful next bounded task.

## Validation

Implementation `e3f4414` passes Debug, Release and ASan/UBSan (103/103 each)
on Apple M4 Max with Xcode 26.6. Metal-disabled Apple Clang and LLVM 18 builds
pass 75/75 each; all 102 Python checks pass. The new portable scene tests, independent GPU math/transport checks, all 47 measured preset
comparisons and eight matched PBRT image fixtures pass. The matched images cover
120 regional RGB gates across 256 renders; their raw PFMs are independently
recomputed. Coefficient tables match all 282 source RGB values exactly;
reconstructed spectra pass the existing RGB and blackbody tolerances.

Metal API validation passes all 30 suites without skips. All 11 affected
shader-instrumented suites pass with an explicit 600-second diagnostic budget;
normal PR deadlines and numerical gates are unchanged. Changed-unit Clang-Tidy,
native Clang analysis and repository hooks pass. Four owned showcase scenes illustrate absorption, phase anisotropy, emission/nested
media and measured presets, with separate raw EXR and display PNG output.

Earlier evidence and numerical caveats remain in the
[surface](surface-material-validation.md), [dielectric](dielectric-validation.md),
[conductor](conductor-validation.md), [HDR](hdr-image-validation.md) and
[texture](core-texture-validation.md) records. Their counts describe those
earlier revisions.
