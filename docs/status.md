# Project status

## Current milestone

[Linear HDR image I/O (PR #28)](https://github.com/rlefko/rypath/pull/28) is merged
at `edaeb6da`. It adds bounded HALF/FLOAT EXR and Radiance RGBE inputs, binary32
texture/light storage, and atomic linear EXR film output.

Spectral GGX conductors are ready for review. The implementation adds smooth
and anisotropic single-scattering reflection, unclamped named/numeric/file eta/k
spectra, scalar roughness textures, and mapped anisotropic shading frames.
Area/environment lighting and MIS handle both continuous and delta reflection.
Invalid optical inputs fail preflight; invalid evaluated roughness or numerical
failures reject the entire staged Film batch. See the
[render profile](pbrt-rendering.md) and [conductor validation](conductor-validation.md).

Dielectric transmission, fractional emitting alpha, Ptex, wider color spaces,
additional importers, media, checkpointing and the interactive app remain on the
[roadmap](roadmap.md). This slice does not establish offline MVP completion.

## Validation

The conductor implementation and grazing-frame follow-up pass 91/91 Debug,
Release and ASan/UBSan tests on
Apple M4 Max, including native transport, mapped frames, exact sample/Film
partitioning, a captured grazing-sample regression and whole-batch rejection.
Portable Apple Clang and Clang 18
builds pass 70/70 tests. Independent BSDF checks cover 5,493 point/sample cases,
128 reciprocity and axis-swap pairs, and 524,288 energy/probability samples.
All 23 Metal tests pass API validation, 79 Python checks pass, and the final
eight-scene reference comparison passes 120 gates across 256 clean renderer
runs. The conductor record identifies the explicit roughness comparison
override, shader-instrumentation timeouts and longer diagnostic runs, static
analysis, environment and numerical limitations.
The initial 23-test shader validation required three longer diagnostic reruns;
the follow-up passes both affected transport suites under instrumentation.
Normal CTest limits are
unchanged. The owned 960×600 conductor showcase completes 4,096 samples per
pixel (2.36 billion total), without denoising.

A stable checker integral prevents valid nonnegative roughness from becoming
negative through cancellation. Four of 6,440 pinned source-float graph values
remain outside the original comparison threshold; independent integration
supports the new results within `4e-8`. These are explicit numerical differences,
not 6,440 passing comparisons. PBRT runs that substitute black for invalid
samples are excluded from image-agreement evidence.

Historical HDR and texture results remain in [HDR validation](hdr-image-validation.md),
[core texture validation](core-texture-validation.md) and
[performance](core-texture-performance.md). Their earlier test counts describe
those completed slices. Selected external asset coverage remains distinct from
full pinned-corpus support.
