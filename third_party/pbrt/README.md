# PBRT v4 source and data

## Image texture adaptations

At the same pinned revision below, `src/image_texture.cpp` adapts PNG transfer
curves, resampling, pyramid generation, and point/bilinear/trilinear/EWA mip lookup from
[image.cpp](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/image.cpp),
[color.cpp](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/color.cpp),
[color.h](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/color.h),
and [mipmap.cpp](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/mipmap.cpp).
Sixteen-bit PNG inputs follow the source and mip binary16 rounding contract in
`image.cpp`; the continuous sRGB rational approximation comes from `color.h`.
Portable integer binary16 conversion follows nearest-even rounding, as does
[float.h](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/float.h).
`shaders/image_texture.h` adapts UV lookup, mip selection, albedo processing,
and the RGB-to-spectrum lookup to packed Metal buffers. It follows
[textures.cpp](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/textures.cpp),
`mipmap.cpp`, and the sRGB reconstruction sources below.
`src/ewa_weights.h` retains the pinned 128-entry EWA weight table for both
backends. Stable covariance arithmetic and bounded coordinate handling are
rypath numerical adaptations described in the [EWA record](../../docs/ewa-texture-validation.md).

`src/texture_footprint.cpp` and `shaders/texture_footprint.h` adapt pinhole
differentials, triangle and bilinear partials, and the UV derivative solve.
rypath uses world-space geometry and prepares the camera's fallback differentials
on the host. The corresponding sources are
[cameras.cpp](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/cameras.cpp),
[cameras.h](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/cameras.h),
[shapes.h](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/shapes.h),
and [interaction.cpp](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/interaction.cpp).
The adaptations retain PBRT's copyright and [Apache-2.0 license](LICENSE.txt).
The [image texture validation record](../../docs/image-texture-validation.md)
describes the numerical contract.

## Perspective thin-lens adaptation

`shaders/render.metal` adapts the perspective thin-lens ray construction and
shared-aperture ray differentials from pinned
[cameras.cpp](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/cameras.cpp).
Its uniform concentric disk mapping follows
[util/sampling.h](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/sampling.h)
at the same revision, under the upstream copyright and [Apache-2.0 terms](LICENSE.txt).
The normalized aperture retains unit camera weight. The `lens-v1` sampling
extension reserves Sobol dimensions 1022/1023 and an independent Philox counter
block without changing existing draws; that assignment is rypath's adaptation.
Stable scaled ray construction, reuse of the realized binary32 world-space
aperture displacement for rays and derivatives, and full-support numerical bounds
are also rypath adaptations. See
[depth-of-field validation](../../docs/depth-of-field-validation.md) for limits
and evidence; these adaptations do not establish realistic-lens support.

## Core texture and surface adaptations

`shaders/texture_graph.h` adapts typed texture evaluation, mappings, checkerboard,
dots, marble, and noise from pinned `textures.h`, `textures.cpp`, and
`util/noise.cpp`. The bounded graph representation and validation are original
rypath code. `shaders/surface_detail.h` adapts definition-frame shading partials,
normal mapping, and bump finite differences from `shapes.h`, `interaction.h`,
`materials.h`, and `util/transform.cpp`. `src/mesh_displacement.cpp` adapts
longest-edge refinement, shared midpoints, displacement and normal construction
from `util/mesh.h`, with explicit work/scratch limits and stable host arithmetic.

`shaders/alpha_hash.h` adapts the packed-ray MurmurHash64A convention from
`util/hash.h`; the native query traversal is rypath code. `shaders/image_lighting.h`
adapts equal-area projection, native light image addressing, and RGBIlluminant
reconstruction from `util/vecmath.h`, `lights.h`, and `util/spectrum.h`.
Projection lookup follows the perspective mapping, nearest image addressing and
RGBIlluminant definition in `lights.cpp`, `util/transform.cpp` and `util/image.h`.
Checked affine arithmetic and widened photometric sums are rypath adaptations. The bounded
CDF mixture and its matched PDF are rypath's proposal, not PBRT's sampler.
All cited sources use revision `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f` and retain
the upstream copyright and [Apache-2.0 terms](LICENSE.txt).

The [core texture record](../../docs/core-texture-validation.md) documents
semantic differences, numerical limits, and standalone reference commands.

## Extended surface adaptations

`shaders/layered.h` and `shaders/transmission.h` adapt the finite layered,
thin-dielectric and diffuse-transmission equations from `src/pbrt/bxdfs.h`,
`bxdfs.cpp`, `util/scattering.h` and the HG phase model. `shaders/table_bsdf.h`
adapts measured tensor coordinate mappings and piecewise bilinear sampling from
`bxdfs.cpp` and `util/sampling.h`. Optical RGB texture reconstruction follows
`util/spectrum.h`. These use the same pinned revision and Apache-2.0 terms above.

The scalar-lane Metal implementations, checked packed buffers, material-local
random streams and fixed density-only layered stream are rypath adaptations.
See [surface validation](../../docs/surface-material-validation.md) for their
numerical contract. Legacy Fourier sources have a separate
[PBRT v3 notice](../pbrt-v3/README.md).

## Bilinear intersection adaptation

`shaders/bilinear.h` adapts the bilinear ruling intersection and compensated
products from `src/pbrt/shapes.h` and `src/pbrt/util/math.h` at the same pinned
revision below. It retains Apache-2.0 attribution. The Metal adaptation projects
the surface onto the ray-orthogonal coordinate equations, uses compensated
polynomial arithmetic and a conditioned ruling selection, and honors explicit
ray intervals. Host geometry regularity checks and independent test oracles are
original rypath code. The [validation record](../../docs/pbrt-ply-validation.md)
documents numerical bounds and differences.

## Parser adaptation

`src/pbrt_parser.cpp` adapts the tokenizer, escape decoding, directive grammar,
and Include tokenizer-stack behavior from
[pinned PBRT v4 parser.cpp](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/parser.cpp)
at revision `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`.

PBRT is Copyright (c) 1998–2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
The adapted file retains the Apache License, Version 2.0. See [LICENSE.txt](LICENSE.txt).

rypath modifications include owned source locations and tokens, typed values with
original numeric lexemes, locale-independent decimal conversion, bounded reads
and parsing, and preflight diagnostics. The pinned implementation spells
three-component normals `normal`. rypath also accepts `normal3`, the spelling
listed in the [format reference](https://www.pbrt.org/fileformat-v4), and retains
the authored spelling. Both the pinned implementation's `Option "name" value`
and the documented `Option "type name" value` are accepted. The parser is
standalone C++20 with no PBRT linkage.

## PBRT v4 sRGB reconstruction and D65

[PBRT v4 b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f)
provides the [rgb2spec optimizer](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/cmd/rgb2spec_opt.cpp),
[table lookup](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/color.cpp),
[sigmoid](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/color.h),
and [D65 and CIE Y samples](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/spectrum.cpp).
The reconstruction uses Jakob and Hanika's [A Low-Dimensional Function Space for Efficient Spectral Upsampling](https://rgl.epfl.ch/publications/Jakob2019Spectral).

The data and adapted lookup in `src/rgb_spectrum.cpp` retain the PBRT
Apache License, Version 2.0 and copyright above. PBRT's spectrum source also
credits PhysLight contributions by Anders Langlands and Luca Fascione,
Copyright (c) 2020, Weta Digital, Ltd. See [LICENSE.txt](LICENSE.txt).

`srgb.bin` contains 64 Z nodes followed by coefficients in PBRT's order
`[max channel][z][y][x][coefficient]`, with dimensions `[3][64][64][64][3]`.
Every value is IEEE 754 binary32 in little-endian byte order. There is no
header or padding. The 9,437,440-byte file has SHA-256
`6adffc41eefcf064b6524606cb8734941aa69ac962ac268fd9276f440cef61d5`.
CMake verifies the hash and embeds the data as escaped C++ string chunks.
rypath decodes each word explicitly and retains PBRT's interpolation and
tie behavior. The evaluation uses exact constants for equal channels and
rescales the sigmoid for large polynomial values.

`d65.inc` contains PBRT's normalized `stdillum-D65` evaluated from 360
through 830 nm in 1 nm increments. The extraction uses PBRT's 5 nm D65
knots and its 1 nm CIE Y table, applies the `CIE_Y_integral / InnerProduct`
scale to the knots in PBRT's binary32 arithmetic, and linearly interpolates
the scaled knots. All 471 extracted values matched the pinned build's
`GetNamedSpectrum("stdillum-D65")` at the integer wavelengths bit-for-bit.
The include has SHA-256
`5a9a9c41d16cd61a936704cd5194ec0d2ade4f5f64c728419e2befaac566006a`.
The archived include is a regression reference. The runtime D65 accessor
and built-in name lookup share the same samples from `named_spectra.inc`.

### Reproducing the data

The canonical table uses the pinned CPU PBRT Release build with
`PBRT_FLOAT_AS_DOUBLE=OFF` and `PBRT_USE_PREGENERATED_RGB_TO_SPECTRUM_TABLES=OFF`.
The optimizer was built with Apple Clang 21.0.0 (clang-2100.1.1.101),
Xcode 26.6 (17F113), on arm64 macOS 26.6 (25G5065a).
The generated `rgbspectrum_srgb.cpp` has SHA-256
`adc5a39d555761307987b8b307b607fd848415edd85263f3635c28b0c7236102`.
Optimizer results can depend on the build environment. The extraction
script checks the input and output hashes against this canonical build.

```sh
PBRT_SOURCE=.cache/pbrt-reference/source
PBRT_BUILD=.cache/pbrt-reference/build
"$PBRT_BUILD/rgb2spec_opt" 64 "$PBRT_BUILD/rgbspectrum_srgb.cpp" sRGB
python3 scripts/extract_pbrt_rgb.py \
    "$PBRT_SOURCE" "$PBRT_BUILD/rgbspectrum_srgb.cpp" third_party/pbrt
```

The reference spectra in `tests/rgb_spectrum_tests.cpp` were evaluated by
initializing `pbrt::Spectra` and `pbrt::RGBToSpectrumTable` in a standalone
program linked to that same PBRT build's `pbrt_lib`. For each listed
chromatic input, the program used
`(*pbrt::RGBToSpectrumTable::sRGB)(pbrt::RGB(r, g, b))` and evaluated the
returned polynomial at the listed wavelengths. The normal rypath build
uses the checked-in data with no Python, PBRT, or network dependency.

## PBRT v4 built-in named spectra

`named_spectra.inc` contains all 88 names in the same pinned PBRT v4
[spectrum.cpp registry](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/spectrum.cpp#L2661),
with one row of 471 IEEE 754 binary32 samples for each name at integer
wavelengths from 360 through 830 nm. The names are case-sensitive. CMake
checks SHA-256 `1ad23d63bc8e02a3f88e22dfe660c07114d037a32d1f8d8666c9723400c833bc`
and compiles the checked-in hexadecimal literals into static data.

The separate `named_spectrum_knots.inc` contains the prepared binary32
wavelength/value pairs for the same 88 names, including fractional
wavelengths and knots outside the visible range. Diffuse reflectance uses
this piecewise-linear representation. CMake checks SHA-256
`92c84ba72972a7eb2f7d148f39aece27e9860d4d790021878621ca44c4f53cdb`.

The extractor follows PBRT's `FromInterleaved` and `PiecewiseLinearSpectrum`
arithmetic. If the knots leave either visible endpoint uncovered, PBRT
extends the endpoint value using a knot at 359 or 831 nm. For the 16
illuminants marked for normalization, it sums the samples weighted by
PBRT's CIE Y table in binary32 and scales the knots by
`CIE_Y_integral / InnerProduct` before resampling. This is the upstream
spectra initialization step. rypath applies its light calibration separately
when translating a named spectrum used as emission.

Both generated includes retain PBRT's Apache-2.0 attribution and the
PhysLight measurements notice for Anders Langlands and Luca Fascione,
Copyright (c) 2020, Weta Digital, Ltd. The optical glass data are from
[refractiveindex.info](https://refractiveindex.info) under
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/).

### Reproducing and validating the named data

From the repository root, use a CPU Release PBRT build at the pinned
revision with `PBRT_FLOAT_AS_DOUBLE=OFF`. The following link command applies
to the macOS PBRT build described above, with OpenEXR exposed through
`pkg-config`.

```sh
pbrt_source="$PWD/.cache/pbrt-reference/source"
pbrt_build="$PWD/.cache/pbrt-reference/build"
validation_dir=build/named-emission-validation
mkdir -p "$validation_dir"
python3 scripts/extract_pbrt_named_spectra.py "$pbrt_source" "$validation_dir/named_spectra.inc" \
    --knots-output "$validation_dir/named_spectrum_knots.inc"
cmp third_party/pbrt/named_spectra.inc "$validation_dir/named_spectra.inc"
cmp third_party/pbrt/named_spectrum_knots.inc "$validation_dir/named_spectrum_knots.inc"

c++ -std=c++17 -ffp-contract=off -I"$pbrt_source/src" -I"$pbrt_source/src/ext" -I"$pbrt_build" \
    scripts/pbrt_named_spectrum_probe.cpp "$pbrt_build/libpbrt_lib.a" \
    "$pbrt_build/src/ext/libdeflate/libdeflate.a" \
    "$pbrt_build/src/ext/double-conversion/libdouble-conversion.a" \
    "$pbrt_build/src/ext/utf8proc/libutf8proc.a" \
    $(pkg-config --libs OpenEXR) -o "$validation_dir/pbrt_named_spectrum_probe"

cmake --preset debug -B build/named-data -DRYPATH_ENABLE_METAL=OFF
cmake --build build/named-data --target rypath_pbrt_spectra_tests
python3 scripts/compare_pbrt_named_spectra.py "$pbrt_source" \
    "$validation_dir/pbrt_named_spectrum_probe" build/named-data/tests/rypath_pbrt_spectra_tests \
    --output-directory "$validation_dir"
```

The comparator derives the list of names directly from the pinned source
and compares the compiled rypath lookup to calls to PBRT's
`Spectra::Init` and `GetNamedSpectrum`. On Apple M4 Max with the compiler
and macOS versions above, all 88 names and 41,448 sample words matched
bit-for-bit. Both output dumps had SHA-256
`3bcc076b37c15bb3c714937090ecf3ce2b04d8436c25220ab01d1af9c52360ac`.

## Conductor adaptation

`shaders/conductor.h` adapts `ConductorBxDF`, `TrowbridgeReitzDistribution`, and
complex Fresnel from the same pinned PBRT revision. It retains the Apache-2.0
notice and license. Roughness remapping, the effectively-smooth boundary, the
rough-axis floor, correlated Smith masking-shadowing, and visible-normal
sampling follow that implementation. The distribution sampler is based on
[Heitz 2018](https://jcgt.org/published/0007/04/01/).

Checked Metal arithmetic and stable optical-limit/VNDF endpoint calculations are
rypath adaptations, independently tested against double equations and pinned
PBRT over the ordinary domain. `scripts/pbrt_conductor_probe.cpp` is an optional
external oracle linked to the cached CPU reference; no PBRT renderer dependency
is introduced into the product. See the [validation record](../../docs/conductor-validation.md).

## Dielectric adaptation

`shaders/dielectric.h` adapts the same pinned PBRT `DielectricBxDF`, Fresnel and
Snell equations, retaining Apache-2.0 attribution and reusing the conductor's
checked GGX helpers. The original scalar-wavelength transport samples one
independent Fresnel lobe choice per interface. Compensated critical-angle
arithmetic and rationalized refraction are rypath numerical adaptations.
`scripts/pbrt_dielectric_probe.cpp` is an opt-in oracle against the external
pinned CPU build, not a production dependency. See
[dielectric validation](../../docs/dielectric-validation.md).

## Homogeneous measured-medium presets

`medium_presets.inc` retains all 47 exact case-sensitive names and the measured
reduced-scattering/absorption RGB values from
[`media.cpp` at b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/media.cpp).
It retains PBRT's Apache-2.0 copyright and license; see [LICENSE.txt](LICENSE.txt).
The source credits Jensen, Marschner, Levoy and Hanrahan, *A Practical Model for
Subsurface Light Transport* (SIGGRAPH 2001), and Narasimhan, Gupta, Donner,
Ramamoorthi, Nayar and Jensen, *Acquiring Scattering Properties of Participating
Media by Dilution* (SIGGRAPH 2006).

Rows retain the upstream order: name, reduced-scattering RGB, absorption RGB.
The table documents mm⁻¹ units. Like pinned PBRT, homogeneous preparation uses
the reduced-scattering values directly as `sigma_s`, reconstructs both spectra
using sRGB Unbounded, and applies only the authored coefficient scale; it does
not convert units or divide by `(1-g)`. The table is compiled locally and needs
no runtime asset, download or PBRT library. The optional
`scripts/pbrt_medium_probe.cpp` validates dense preparation against the cached
pinned CPU PBRT build; production remains independent of that reference.

## Area-light power

`src/area_power.cpp` adapts the rectangle classification and fixed 3×3 bilinear
patch area estimate from PBRT's `BilinearPatch` at the same pinned revision,
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`. The source retains PBRT's Apache-2.0
copyright and license notice; see [LICENSE.txt](LICENSE.txt). Preparation uses
binary64 arithmetic over rypath's rounded world-space binary32 vertices, a
documented numerical adaptation rather than bit-identical upstream arithmetic.
The estimator's local area PDFs remain independent of this normalization area.
See [area-power validation](../../docs/area-power-validation.md).

## Loop subdivision

`src/loop_subdivision.cpp` adapts the Loop stencils, limit projection and raw
tangent-cross-product normals from `src/pbrt/util/loopsubdiv.cpp` at the same
pinned revision, `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`. The source retains
the upstream copyright and Apache-2.0 notice; see [LICENSE.txt](LICENSE.txt).
Rypath adds checked indexed topology, deterministic allocation bounds and
source-located diagnostics. It rejects malformed manifold topology instead of
depending on upstream's unchecked adjacency assumptions. No PBRT library is
linked into the product. The optional geometry oracle links the existing cached
CPU reference separately; see [mesh validation](../../docs/mesh-shape-validation.md).

## Sampling and Gaussian reconstruction

`src/qmc_shared.h` adapts Halton and ZSobol indexing, radical inverses, hashing
and randomization from the same pinned PBRT implementation. `primes.inc`
retains its 1,000 prime bases; see [sampling provenance](sampling.md).
`src/gaussian_filter.cpp` adapts the Gaussian midpoint tabulation, `FastExp`
and piecewise-constant CDF construction. Both retain Apache-2.0 notices.
The normalized positive table uses unit film weights, a documented numerical
adaptation. Optional probes link the cached CPU reference; the product adds no
PBRT runtime dependency.
