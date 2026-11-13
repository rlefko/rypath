# PBRT image texture validation

## Reflectance and footprint

PNG imagemaps extend the diffuse albedo model to vary with surface location and
texture footprint. Filtering and spectral reconstruction are deterministic
functions of the existing hit, camera sample, declared total samples per pixel,
and path wavelength. The diffuse BSDF and its sampling density keep their
existing form with the evaluated albedo. The lookup consumes no random
dimensions and adds no ray queries. Raw spectral XYZ accumulation and PNG
presentation retain separate storage.

The [structural profile](pbrt-profile.md#png-imagemap-definitions) defines supported
PNG inputs and memory limits. The portable decoder is the bounded
[LodePNG adaptation](../third_party/lodepng/README.md). Power-of-two sources
preserve the encoded base level. Other accepted sizes resample to the next
power of two on each axis using separable four-tap windowed sinc, with repeat or
clamp addressing. Each next mip level averages a 2×2 footprint of the previous
linear RGB working level. The stored mip bytes are encoded independently
at each level. Their quantization does not propagate through downsampling.
[PBRT image and mip adaptations](../third_party/pbrt/README.md)

The sampler computes a footprint width of twice the largest absolute mapped UV
derivative. The mip index is the floor of
`level_count - 1 + log2(max(width, 1e-8))`, bounded to the available levels.
Sampling uses bilinear interpolation within the selected mip. At or above the
coarsest level it returns that level's single texel directly. Transfer decoding
precedes interpolation. Texture scale, optional inversion, and albedo clamping
precede sRGB spectral reconstruction.
[Pinned PBRT mip selection](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/mipmap.cpp)

The primary hit footprint intersects the two offset pinhole directions with
the geometric tangent plane. Secondary hits and degenerate primary projections
use a camera approximation with minimum direction differentials computed on
the host. The differential scale is `max(0.125, 1/sqrt(total_spp))`, independent
of batch boundaries. The UV derivative solve projects position differentials
onto the triangle or bilinear patch texture partials. Triangle partials are
computed after the camera-relative shape transform and carried through the object
placement. Rebase translations before narrowing frame vertices and reconstruct
camera-relative hit positions before rounding to world coordinates.
Degenerate triangle UVs use a tangent frame in the triangle plane. The shader
evaluates patch partials at the hit's patch parameters and preserves the
separate interpolated texture UV.
[PBRT camera and surface differential adaptations](../third_party/pbrt/README.md)

## Focused Debug evidence

The development Debug build ran on Apple M4 Max (`applegpu_g16s`), macOS 26.6
(25G5065a). The four image fixtures ran through the product CLI at their
authored 8×8 resolution and 4,096 samples per pixel. Each run committed
262,144 samples and wrote an RGB PNG. The same fixtures were compared before
display conversion with the pinned PBRT Release/binary32 CPU reference using
the [existing comparison harness](../scripts/compare_pbrt.py). Each backend
ran 16 fixed seeds (rypath 0–15 and PBRT 16–31) at 4,096 samples per pixel.
Texture filtering stayed enabled. The gates used five combined standard
errors plus `2e-5 * max(1, abs(reference))`.

| Fixture | Coverage | Passed gates | Largest absolute RGB mean difference |
| --- | --- | ---: | ---: |
| Magnified | 3×5 image, authored/default UVs, mirrored placement, linear/sRGB, scale and inversion | 15/15 | 0.002686 |
| Minified | Intermediate-level and coarsest-level regions | 12/12 | 0.0008352 |
| PLY | Curved bilinear patch and triangle with an unmodified PNGSuite image | 21/21 | 0.006114 |
| Secondary | Off-camera textured diffuse panel seen by secondary rays | 15/15 | 0.002855 |

All 63 gates passed across 128 linear PFM outputs. The largest difference
relative to its threshold was 0.331. The [fixture provenance and checksums](../tests/fixtures/pbrt-render/image-assets/README.md)
identify the external PNG and the owned wrappers. The PBRT executable hash
matches the [pinned reference build record](pbrt-render-validation.md#pinned-upstream-reference).
The Debug rypath reference executable SHA-256 was
`d8ed7478513d74d454fe2375466a033eb814aaef38f636ca465dbcb720fdb3be`; the
Metal library SHA-256 was
`b443e879cd4c4d6d91e6e55e683a058133d2db89da9a7bb84acf692b56c64f04`.

A separate encoded mip comparison against pinned PBRT found two channel bytes
differing by one code among 832,332 checked bytes. That check documents a
quantization difference at byte precision. The render comparisons use linear
RGB region means and do not require bitwise mip or image identity.

## Completed implementation validation

On 2026-11-13 UTC, the implementation recorded in `ff5e3d2` (based on main
`d57f89d`) passed the full
Release comparison: **348/348 gates, 27 fixtures, 864 PFM outputs**. The four
image fixtures retained the results above to the reported precision. The Release
reference executable SHA-256 was
`1ee62657f7e11bd4fefafd03517f05f40370065af877344f60ade89b4106a353`;
the Metal library was
`185efaa41add8cb5bff9eaa34f2e908921abdf77782a48024863cebed450b444`.
Hardware was Apple M4 Max, macOS 26.6 (25G5065a), Xcode 26.6 (17F113), with
Metal compiler 32023.883.

- Debug, Release, and ASan/UBSan: 70/70 CTest tests each, Metal enabled and both
  availability skips disabled.
- Metal-disabled ASan/UBSan: 58/58 tests.
- Metal API and shader validation: 14/14 native tests with `MTL_DEBUG_LAYER=1`
  and `MTL_SHADER_VALIDATION=1`.
- Native filtering agrees with a same-seed linear RGB control to `7.16e-7`
  maximum absolute path radiance. Incorrect encoded-RGB and spectral-space
  interpolation differ by more than 0.21 and 0.58 respectively.
- Very wide finite mappings (`3e38`) select the coarsest mip without arithmetic
  failure. A shared camera/geometry translation of `1e8` preserves the primary
  footprint. Both regressions have zero maximum raw-path difference from their
  controls. Invalid camera matrix pairs are rejected before upload.
- Batch sizes 1, 7, and 65 give exactly equal raw films for 133,120 paths,
  crossing the dispatch capacity. The footprint probe checks 96 vectors spanning
  primary/secondary projections, patch and triangle UVs, and four sample budgets.
- All 43 project C++ units pass LLVM 18 clang-tidy; the vendored LodePNG unit is
  excluded from project lint and remains compiled under sanitizers and CodeQL.
  All Objective-C++ units pass Apple Clang Static Analyzer. Python policy and
  comparison-harness tests pass 25/25. Repository hooks pass.

The full comparison uses the command in [the reference record](pbrt-render-validation.md).
Native regression commands are `ctest --preset debug -R 'image|texture|png'` and
`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --preset debug -R '^metal\.'`.
Performance and full-corpus coverage were outside these checks. Hosted CI is
reported on the PR separately from these local hardware measurements.

The subsequent decoder-only fix widens internal PNG padding and Adam7 offset
arithmetic before multiplication. These paths are outside the accepted 8-bit,
noninterlaced input profile. A standalone ASan/UBSan probe checked offsets above
`UINT32_MAX` without allocating image storage and exact 1-bit PNG round trips
with and without Adam7. The final source also passed 70/70 tests in each of Debug,
Release, and ASan/UBSan, 58/58 in Metal-disabled ASan/UBSan, and 14/14 with Metal
API/shader validation. The comparison artifact hashes above identify the prior
implementation; no transport or supported texture arithmetic changed in this fix.
