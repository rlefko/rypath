# PBRT triangle-filter validation

This slice extends pixel reconstruction in the [PBRT rendering profile](pbrt-rendering.md).
It changes the camera-sample distribution and the filtered image integral.
It preserves wavelength sampling, transport, spectral calibration, raw film
accounting, and the original half-pixel box/demo camera paths. Numerical tests
supply bounded evidence; they do not establish general PBRT equivalence.

## Sampling and numerical contract

For radius r, the one-dimensional normalized density is
`max(0, r - abs(d)) / (r*r)` on `[-r,r]`. The two axes are independent. The
[pinned PBRT triangle filter](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/filters.h)
samples this density with unit weight. rypath evaluates its inverse as
`s = 2*u - 1; d = r * (s / (1 + sqrt(1 - abs(s))))`, avoiding subtraction near
the center. This is an equivalent continuous inverse, not identical PBRT float
arithmetic. PBRT's [image reconstruction discussion](https://pbr-book.org/4ed/Sampling_and_Reconstruction/Image_Reconstruction)
provides the pixel-centered sampling convention.

The existing camera uniforms use the top 24 bits of Philox block-zero words X/Y:
`float(word >> 8) * 2^-24`. Zero is included and one excluded; the finite grid's
negative endpoint is present and its positive endpoint absent. There are no
extra random dimensions or retries. The two radii default independently to two
and must narrow to positive normal binary32 values. Subnormal or zero radii,
overflow, and negative values are errors. A small accepted radius can round away
when added to a pixel center; no claim of continuous subpixel resolution is made.

Triangle raster coordinates evaluate `((float(pixel) + .5f) + offset) /
float(dimension)`. The direction uses explicit nested FMA:
`fma(y, up, fma(x, right, forward))`, then max-component-scaled normalization.
The half-pixel box and demo branches preserve their earlier expressions.

Every sample retains the original pixel and unit weight, even outside the film.
There is no clipping, splatting, radius multiplier, or edge renormalization.
The normalized distribution does not need a potentially overflowing `r^4`
filter-integral calculation. Camera arithmetic, rather than a fixed radius cap,
limits wide filters.

Portable validation encloses the actual float raster support, including integer
conversion at large dimensions, and both FMA stages. A conservative ray-plane
separation bound rejects directions that could collapse into the subnormal
range. The bounds allow both nearest and toward-zero rounding permitted by the
[Metal language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf),
as well as subnormal flushing; extreme
cancellation can be rejected even when individual sampled rays would survive.
Translation reports these errors at the effective filter declaration before
acquiring a device. The progressive driver repeats the dimension-aware check,
and a native session checks each request using its owned packed camera/filter.
Runtime invalid arithmetic still rejects the entire pending batch. There is no
borrowed construction storage or selectively discarded path.

## Acceptance criteria fixed before measurement

Portable tests cover effective defaults and precedence, include provenance,
unknown parameters, invalid narrowing, independently chosen radii, wide
footprints, dimension-sensitive bounds, intermediate-only overflow, and large
integer dimensions. An independent double CDF checks the inverse with normalized
error at most `8 * FLT_EPSILON`.

Two depth-zero GPU fixtures use black diffuse surfaces with unit emission and
no environment. A 1×1 camera maps tent offsets to `(x,-y,1)` on the emitter
plane. The first uses radii (1,1) and an emitter beginning at x=.75, giving
probability 1/32. The second uses radii (1.5,.75), x>=.75 and world y>=.25,
giving probability 1/36. All contributing rays lie outside the nominal film.
Each uses 32,768 paths, with fixed seeds `0x36b2adcf517ee289` and
`0xd825e489e71fb063`. The probability gate is
`6 * sqrt(P*(1-P)/32768) + 2e-5`; every sample contributes to the mean. Half-pixel
box controls must be exactly black. These geometric integrals do not reuse the
production inverse or Metal packing.

Constant-sky tests require unchanged per-sample random words, wavelength,
radiance, and count under the default radius-two filter. Native checks also
cover workloads of 1 and 257, stable identities, padded guards, destroyed input
storage, and preservation/recovery after a dimension-dependent rejected batch.

The new owned asymmetric HDR fixture uses radii (1.5,.75), two unequal colored
emitter rectangles, and a constant colored background. It tests x/y orientation,
anisotropic edge smoothing, and normalization. Its outer emitter edges exceed
the footprint, so the 1×1 native oracles separately establish beyond-film
coverage. All ten existing fixtures, including the CC0
Cornell adaptation, remain byte-for-byte unchanged. The original 7,253-byte
Cornell remains unsupported: one Sobol and eight each normal/UV diagnostics
(17 total). Its triangle filter is now accepted.

All eleven reference fixtures use 8×8 pixels, 4,096 spp, and sixteen fixed seeds
per renderer: rypath 0–15, pinned CPU PBRT 16–31. This is 46,137,344 samples per
renderer. The new fixture adds full-frame and four-quadrant RGB comparisons for
117 total gates. Each requires five combined standard errors plus
`2e-5 * max(1, abs(reference))`. Seeds and thresholds are not changed to pass.
Both renderers expose unquantized linear RGB with savefp16=false; PFM decoding
retains negative/above-one values and handles row order and endianness.

## Evidence

Hardware: Apple M4 Max, 16 CPU cores, 40 GPU cores, 128 GB unified memory;
macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang 21.0.0
(`clang-2100.1.1.101`), CMake 4.0.3, Ninja 1.13.2. Deployment remains
macOS 14 / MSL 3.1. The system-default device is `applegpu_g16s` with compute
ray tracing. The clean pinned reference source and submodules are unchanged;
CPU PBRT binary SHA-256 is
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.

| Check | Result |
| --- | --- |
| M4 Max Debug, Release, ASan/UBSan | 52/52 CTests each, no skips; 3.88 / 3.39 / 8.20 seconds |
| Metal API and shader validation | 8/8 native CTests, no skips; 1.28 seconds |
| Metal-disabled ASan/UBSan | 46/46 portable/stub CTests; 4.83 seconds |
| LLVM 18 clang-tidy / Apple static analyzer | All 23 C++ / five Objective-C++ sources passed |
| Hooks and Python policy/reference tools | Changed-file hooks and 14 Python tests passed |
| Pinned CPU PBRT comparisons | 117/117 gates, 352 PFM outputs; 22.24 seconds total wall time |

The analytic edge case returned 999/32,768 hits (0.0304870605469), compared with
1/32 and an absolute gate of 0.00578709561085. The corner returned 843/32,768
(0.0257263183594), compared with 1/36 and a gate of 0.00546700538108.
Independent analytic thresholds applied to the fixed Philox words reproduced
both exact counts. Box controls were exactly black, and the constant-sky sample
streams and raw-film batch comparisons were identical.

The new reference fixture's full-frame linear RGB means were
(0.85631025, 0.45745349, 0.44614961) for rypath and
(0.85705471, 0.45760335, 0.44671121) for PBRT. Its maximum error/bound ratio was
0.2914 (top-left red). The overall maximum was 0.6255 in the existing RGB
multibounce bottom-right red gate. The 320 outputs from the original ten
fixtures were byte-identical to their prior area-light validation outputs.

Independent reviews checked the float support bounds, packed ownership, inverse
sampling, camera orientation, CDF and coverage oracles, fixture hashes, and all
352 outputs. Review sharpened the intermediate-only overflow test, widened the
arithmetic bound to cover Metal's permitted rounding modes, and clarified which
tests establish beyond-film coverage. These are validation-run timings, not
renderer performance claims.

Routine CTest remains offline. The pinned CPU Release/float reference stays in
an ignored cache and is not a runtime dependency. Commands and dependency
provenance are in the [spectral validation record](pbrt-render-validation.md).
Hosted availability and CI duration will be recorded against the final PR
revision; explicit hosted skips do not replace strict M4 Max execution.
