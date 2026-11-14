# PBRT packed grayscale PNG validation

## Compatibility contract

Diffuse spectrum imagemaps accept noninterlaced grayscale PNGs at 1-, 2-, and
4-bit sample depth. Each sample expands to three identical RGB8 bytes using
`sample * 255 / (2^depth - 1)`. This is exact for every supported sample code.
Transparency keys do not change reflectance, including keyed white samples.
PNG gamma, background, and color metadata remain inert; the texture's default
`sRGB` or explicit `linear` encoding selects the existing eight-bit transfer.

This follows the CPU/float
[pinned PBRT reader](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/image.cpp#L1277-L1302).
PBRT retains a grayscale channel while rypath replicates it into RGB. Equal RGB
channels evaluate to the same scalar reflectance through the existing image path.
The [PNG transparency specification](https://www.w3.org/TR/png-3/#11tRNS) allows
unused high key bits to be ignored. rypath validates the key's framing and discards
its value without compositing, so those bits do not affect acceptance.

The existing LodePNG conversion unfilters rows, removes padding, and expands
packed grayscale into RGB8. It needs no vendored changes. Shared source ownership,
encoded-byte mip storage, point/bilinear/trilinear/EWA filtering, and the Metal
descriptor remain unchanged. Interlacing, animation, and non-power-of-two black
wrapping remain outside the profile.

This extends input representation. Random dimensions, sampling densities, MIS,
termination, and raw XYZ accumulation are unchanged.

## Resource and failure behavior

For width W, height H, and sample depth d, scanlines occupy
`H * (1 + ceil(W*d/8))` bytes and unpadded samples occupy `ceil(W*H*d/8)` bytes.
The retained RGB8 source costs `3*W*H` bytes. Existing scratch accounting covers
inflation, unfiltering, RGB conversion, and two live RGB buffers during the copy
into retained storage. Palette overhead is unnecessary for grayscale.

The existing input, dimension, decoder allocation, DEFLATE-work, scratch, and
aggregate retained limits apply before allocation. Malformed input remains an
`invalid-image` error; insufficient resources remain `limit-exceeded`.
Unused definitions are checked and source-located. Neither a partial image nor
a failed render replaces existing output.

## Acceptance criteria

- Independently authored packed images cover every sample value at every depth,
  all five row filters, odd multirow widths, nonzero padding, consecutive split
  IDAT chunks, and nonblack transparency keys.
- Malformed rows, keys, CRCs, and forbidden palettes fail. Exact and one-byte-short
  retained/scratch cases exercise both inflation and RGB-copy limits.
- Independent RGB8 controls match source bytes and stored mip words within each
  backend, under both encodings and supported wraps. Native controls cover all
  four filters, triangles, and bilinear patches with identical sample identities
  and radiance.
- Selected controlled power-of-two mip cases match pinned PBRT U256 storage.
  General filtered comparisons retain the established numerical criteria rather
  than assuming universal cross-renderer bitwise mip identity.
- One owned point wrapper uses unmodified pinned PNGSuite `tbbn0g04.png`. Its
  sixteen grayscale levels, transparent white, black background metadata, and
  gamma metadata distinguish expansion, compositing, and transfer errors. The
  [asset manifest](../tests/fixtures/pbrt-render/image-assets/README.md) retains
  its hash, source, and license.
- Reference renders use 8x8 pixels, 4,096 spp, and sixteen fixed independent seeds
  per backend. Each declared RGB region must satisfy
  `abs(delta) <= 5*combined_SE + 2e-5*max(1, abs(reference))`.

This is selected external-asset coverage in an owned wrapper, not a new
unmodified upstream scene entry point or full-corpus acceptance.

## Reproduction

Configure, build, and test `debug`, `release`, and `asan-ubsan` with both Metal
availability allowances disabled. Also run:

```sh
cmake --preset asan-ubsan -B build/grayscale-decoder -DRYPATH_ENABLE_METAL=OFF
cmake --build build/grayscale-decoder
ctest --test-dir build/grayscale-decoder --output-on-failure --no-tests=error
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --preset debug --no-tests=error -R '^metal\.'
.venv/bin/python -m unittest discover -s scripts -p 'test_*.py'
```

The probes retain the existing `source-u256` and `u256` output contracts. Build
the pinned PBRT probe with the [existing link recipe](image16-texture-validation.md#reproduction),
setting `validation_dir=build/grayscale-validation`. The external-asset comparison
uses Pillow for independent decoding; routine tests stay offline.

```sh
python3 scripts/compare_pbrt_grayscale.py \
  .cache/pbrt-reference/source \
  build/grayscale-validation/pbrt_image16_probe \
  build/release/tests/rypath_image16_probe \
  tests/fixtures/pbrt-render/image-assets \
  --output-directory build/grayscale-validation/exact
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir build/grayscale-validation/reference
```


## Measured evidence

Measured November 14, 2026 for implementation `89d8329`, based on main
`89a7f43`, on Apple M4 Max (`applegpu_g16s`), macOS 26.6 (25G5065a),
Xcode 26.6 (17F113), Apple Clang 21, CMake 4.0.3, and Ninja 1.13.2.
Both Metal availability allowances were disabled.

- Debug, Release, and ASan/UBSan each passed **74/74 CTests**. The separate
  Metal-disabled sanitizer build passed **61/61**. Metal API and shader validation
  passed **15/15**, without skips. Later test-only guard changes passed the
  affected preflight test in all four builds.
- All **71 Python tests**, LLVM 18 clang-tidy on the four changed C++ units,
  formatting, and repository hooks passed.
- The exact probe comparison passed all seven cases: **13,836 source RGB codes**,
  **41,736 within-backend mip codes**, and **396 selected cross-backend mip codes**.
  Both sRGB and linear encodings passed. These are representation checks, separate
  from statistical image comparisons.
- The native two-bit grayscale fixture matched **8,192 path radiances exactly**
  against independent RGB8 controls, with identical sample identities. The cases
  exercise all four filters, both encodings, triangle and bilinear-patch hits,
  and repeat/clamp/black wrapping.
- Independent review used a separate filtered PNG writer and a sanitized decoder
  harness: **336 valid**, **315 malformed**, and **42 unsupported** inputs passed,
  plus six retained/scratch boundary pairs, 4,096-square scratch boundaries at
  each new depth, and the 4,097-axis rejection. Review also verified the pinned
  asset, metadata, sixteen grayscale levels, and exact wrapper texels.

- The full pinned PBRT render suite passed **594 statistical RGB gates** and
  **eight filter sensitivity controls** across **52 fixtures** and **1,672 linear
  outputs**. The new grayscale fixture passed all nine full/transparent-white/
  intermediate-gray gates at the unchanged thresholds. The transparent-white
  reference means were approximately 1; intermediate gray was approximately
  0.0331 after sRGB decoding, distinguishing it from linear 0.2.
- The product CLI published an inspected **256x256 PNG at 256 spp**, with only
  resolution and sample-count overrides to the new owned fixture. It committed
  all **16,777,216 samples**, preserving the expected white and grayscale regions.

Local artifacts are under `build/grayscale-validation/`; they are not tracked.
Hosted Linux, macOS, and CodeQL results are reported on the PR separately.

| Artifact | SHA-256 |
| --- | --- |
| Release CLI | `3eea1e7272399c5d6c40096ff1760a55e9d89352b5c7ae193832f2d2c2461fa1` |
| Release reference renderer | `d4aaf5551ec449bb5758ae5a3a717a77084b76df061c0f4188fef93773b73295` |
| Release image probe | `1d2c3fb5323236b78c637e2ec6aea843651b02be21c71796c36d9312e541ac9b` |
| Pinned PBRT image probe | `f945ba084aa1c2eb40daf89c4a1031209219c64f2df2199e54f93bbdd24aa02a` |
| Pinned PBRT executable | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |
| CLI preview PNG | `b3920fc4ba1661f7eca8763cde2c14186f27830e5c3472de9d423d4bf6bda522` |
