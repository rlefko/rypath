# Project status

## Current milestone

[Spectral GGX conductors (PR #29)](https://github.com/rlefko/rypath/pull/29) is
merged at `8bc47ef`. It adds smooth/anisotropic reflection, raw eta/k spectra,
textured roughness and mapped frames.

Spectral GGX dielectrics are ready for review. This slice adds polished
and frosted glass, spectral refraction, Fresnel lobe selection, both-hemisphere
light/BSDF MIS and authored interface orientation. Raw Film accumulation and
existing diffuse/conductor sampling streams remain separate and unchanged.
Glass Sobol scenes use a dedicated lobe tail and a 113-bounce bound.
See the [render profile](pbrt-rendering.md) and
[dielectric validation](dielectric-validation.md), including the explicit
near-unity binary32 direction/PDF limitation.

Thin/coated materials, absorption and media, fractional emitting alpha, Ptex,
wider color spaces, additional importers, checkpointing and the interactive app
remain on the [roadmap](roadmap.md). This slice does not establish offline MVP
completion or full pinned-corpus support.

The next suggested bounded task is durable checkpoint/resume with cancellation
for fixed-target offline renders.

## Validation

Debug, Release and ASan/UBSan pass 95/95 tests on M4 Max. Both Metal-disabled
Apple Clang and LLVM 18 builds pass 72/72, and 84 Python checks pass. Changed-unit
Clang-Tidy, native static analysis and repository hooks pass. The independent
BSDF probe passes 5,144 double cases, 4,096 pinned PBRT comparisons, reciprocity
and axis checks, and 393,216 energy/probability samples. All 25 Metal API tests
pass, as do all four affected shader-instrumented suites. Eight matched PBRT
fixtures pass 120 gates across 256 diagnostic-clean
renders, independently recomputed from raw PFMs. Instrumentation and numerical
limits are recorded in the dielectric validation record.

Historical conductor checks and numerical caveats remain in
[conductor validation](conductor-validation.md); its timing study is explicitly
[confounded](conductor-performance.md). Earlier HDR and texture evidence remains
in [HDR validation](hdr-image-validation.md),
[core texture validation](core-texture-validation.md) and
[performance](core-texture-performance.md). Their counts describe those revisions.
