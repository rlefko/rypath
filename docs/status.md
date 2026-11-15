# Project status

## Current milestone

[Spectral GGX dielectrics (PR #30)](https://github.com/rlefko/rypath/pull/30) is
merged at `a01bbf2`.

The extended PBRT surface PR adds coated diffuse/conductor, spectral measured
and legacy Fourier tables, thin dielectric, diffuse transmission and nested
material mixes, plus conductor optical textures and reflectance input. Selected
child bump/normal maps, immutable tables and material-local sampling are included.
Finite layered transport and Fourier RGB-to-spectral conversion are explicitly
identified approximations. Raw Film accumulation remains separate.
See the [render profile](pbrt-rendering.md#extended-surface-materials) and
[surface validation record](surface-material-validation.md).

Scene-wide media, arbitrary layers, subsurface/hair, fractional emitting alpha,
Ptex, wider color spaces, additional importers, checkpointing and the interactive
app remain on the [roadmap](roadmap.md). This does not establish offline MVP
completion, general PBRT v3 parsing or full pinned-corpus support. Durable
checkpoint/resume with cancellation remains a useful next bounded task.

## Validation

The current implementation passes Debug, Release and ASan/UBSan (100/100 each),
Metal-disabled Apple Clang and LLVM 18 builds (74/74 each), and 94 Python checks.
Six matched PBRT image cases pass 90 regional RGB gates across 192 renders,
independently recomputed from raw PFMs. Independent layered, measured/Fourier,
texture-role and malformed-input checks are detailed in the surface record.
Metal API validation passes 28/28 without skips. All nine affected
shader-instrumented suites pass with an explicit 600-second
diagnostic budget; normal PR test deadlines and numerical gates are unchanged.
The archived roughglass Fourier asset is explicitly rejected for decreasing CDFs.

Earlier evidence and numerical caveats remain in the
[dielectric](dielectric-validation.md), [conductor](conductor-validation.md),
[HDR](hdr-image-validation.md) and [texture](core-texture-validation.md) records.
Their counts describe those earlier revisions.
