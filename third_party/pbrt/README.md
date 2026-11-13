# PBRT v4 parser source

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
