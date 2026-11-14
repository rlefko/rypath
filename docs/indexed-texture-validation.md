# PBRT indexed PNG texture validation

## Compatibility contract

Diffuse spectrum imagemaps accept noninterlaced indexed PNGs at 1-, 2-, 4-, and
8-bit index depth. Every pixel expands to its exact three RGB8 palette bytes.
The palette's `tRNS` alpha is discarded without compositing, including fully
transparent colors. Missing, empty, and shortened transparency tables do not
alter RGB. Color metadata remains inert; the imagemap's `sRGB` default or
explicit `linear` encoding selects the existing eight-bit transfer function.
This follows the CPU/float
[pinned PBRT PNG reader](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/image.cpp#L1304).

The decoder checks every index against the actual palette entry count. An
undeclared entry is an `invalid-image` diagnostic, rather than the black fallback
of LodePNG's ordinary RGB converter. The
[PNG palette specification](https://www.w3.org/TR/png-3/#11PLTE) permits short
palettes but declares out-of-range pixel indices erroneous. Padding bits at
the end of a source scanline are not pixel indices. Chunk order, CRCs, palette
and transparency lengths, and exact decompressed scanline length retain the
existing validation. Malformed unused imagemaps also fail preflight.

The expanded image is the existing `DecodedImage` with `bit_depth=8`. Sources
and mip pyramids retain their shared ownership and cache keys. Palette inputs
therefore use the same encoded-byte mip storage, point/bilinear/trilinear/EWA
filters, UV mapping, spectral reconstruction, and Metal payload as RGB8 inputs.
Non-power-of-two black wrapping, interlacing, animation, and low-bit-depth
grayscale remain outside the profile.

This extends supported image representation. It changes no random dimensions,
sampling densities, MIS weights, stopping rules, or raw XYZ accumulation.

## Memory and failure behavior

The adapter requests raw indexed output with LodePNG color conversion disabled.
LodePNG unfilters scanlines and removes each row's padding; the resulting indices
are MSB-first and contiguous across rows. For width W, height H, and index depth d,
the scanline payload is `H * (1 + ceil(W*d/8))`, the decoded packed payload is
`ceil(W*H*d/8)`, and the retained RGB payload is `3*W*H` bytes.

Before decoding, the indexed scratch estimate adds 2 KiB for the source and
copied output palettes to the maximum of the existing conservative inflate
estimate, scanlines plus packed indices, and packed indices plus RGB expansion.
Expansion writes directly into the retained RGB vector. Source bytes, dimensions,
decoder allocations, DEFLATE work, scratch, and aggregate retained images/mips
remain under the [existing limits](pbrt-profile.md#files-incomplete-analysis-and-limits).
Allocation failures and budget excesses remain `limit-exceeded`; failed decoding
does not mark a dependency visited or publish a partial image.

## Acceptance criteria

- Independently authored packed images cover every index depth, all five PNG
  row filters, odd multirow widths, nonzero padding, short palettes, and
  transparent nonblack colors. Exact RGB bytes distinguish packing, row order,
  palette selection, and alpha discard.
- An invalid index in a later row fails even when unused padding also contains
  bit patterns outside the palette. Malformed chunks and exact retained/scratch
  boundaries preserve error classification and source locations.
- Indexed and independently expanded RGB8 images produce identical stored mip
  words within each renderer and identical same-seed raw results in rypath. These exact
  representation checks cover supported encodings, wraps, and filters.
- Selected controlled power-of-two mip cases compare exactly with pinned PBRT's
  U256 storage. Broader filtered PBRT comparisons retain the existing numerical
  criteria; the [original image record](image-texture-validation.md) documents
  historical one-code mip-rounding differences.
- The two owned wrappers use unmodified pinned PNGSuite `s35n3p04.png` and
  `tp1n3p08.png`. The former covers odd packed rows and non-power-of-two resizing;
  the latter includes a dedicated transparent-white region that detects black
  compositing. Their [manifest](../tests/fixtures/pbrt-render/image-assets/README.md)
  preserves source, hashes, dependencies, and original license.
- Reference renders use 8×8 pixels, 4,096 spp, and 16 fixed independent seeds per
  renderer. Each declared RGB region must satisfy
  `abs(delta) <= 5*combined_SE + 2e-5*max(1, abs(reference))`.

This is selected asset coverage in owned wrappers, not a new unmodified upstream
scene entry point or full-corpus acceptance. Routine tests remain offline.

## Reproduction

Configure, build, and test the `debug`, `release`, and `asan-ubsan` presets with
both Metal availability allowances disabled. Also run a separate portable
sanitizer build:

```sh
cmake --preset asan-ubsan -B build/indexed-png-portable -DRYPATH_ENABLE_METAL=OFF
cmake --build build/indexed-png-portable
ctest --test-dir build/indexed-png-portable --output-on-failure --no-tests=error
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --preset debug --no-tests=error -R '^metal\.'
.venv/bin/python -m unittest discover -s scripts -p 'test_*.py'
```

The existing rypath and standalone PBRT image probes retain their default
binary16 output. The optional trailing `u256` argument emits encoded RGB8 mip
bytes; `source-u256` emits decoded RGB8 before resizing. Build the standalone
probe using the [pinned reference link recipe](image16-texture-validation.md#reproduction),
setting `validation_dir=build/indexed-png-validation`. The unit-test interpreter
requires the development `emoji` dependency. The extended indexed comparison
below uses a Python interpreter with Pillow for independent external-image
decoding; Pillow is not a production or routine CI dependency.

```sh
python3 scripts/compare_pbrt_indexed.py \
  .cache/pbrt-reference/source \
  build/indexed-png-validation/pbrt_image16_probe \
  build/release/tests/rypath_image16_probe \
  tests/fixtures/pbrt-render/image-assets \
  --output-directory build/indexed-png-validation/exact
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir build/indexed-png-validation/reference
```

## Measured evidence

Measured November 14, 2026 for implementation `de970a8`, based on main
`70da4d4`, on Apple M4 Max (`applegpu_g16s`), macOS 26.6 (25G5065a),
Xcode 26.6 (17F113), Apple Clang 21, CMake 4.0.3, and Ninja 1.13.2.
Both Metal availability allowances were disabled. The extended independent
image decoder was Pillow 12.2.0.

- Debug, Release, and ASan/UBSan each passed **74/74 CTests**. The separate
  Metal-disabled sanitizer build passed **61/61**. Metal API and shader
  validation passed **15/15**, without skips. All **68 Python tests**, focused
  LLVM 18 clang-tidy, all eight Objective-C++ static-analysis units, formatting,
  and repository hooks passed.
- The indexed reference script passed **11 cases**, checking **31,548 source
  RGB codes**, **179,256 within-backend mip RGB codes**, and **528 selected
  cross-backend mip RGB codes** exactly. Every byte value occurs in each color
  channel in the exhaustive palette fixture. Independent expected source bytes
  and Pillow preserve the RGB of transparent entries.
- The default binary16 probe interface retained the existing **540,882 exact
  stored-half comparisons**, plus **196,608 independent linear source-rounding
  comparisons**. The shader artifact is unchanged from PR #24.
- Native triangle and bilinear-patch controls compared **8,192 path radiances**
  exactly with independently expanded RGB8, covering all four filters, both
  encodings, and the three supported wrap modes. Non-power-of-two black wrap
  remains rejected. Existing progressive accounting and failure tests also passed.
- Independent review used a separate PNG writer and sanitized decoder probe:
  **344 valid inputs**, **276 malformed inputs**, **nine limit cases**, and
  **two unsupported cases** passed. Cases include all depths and row filters,
  fragmented IDAT, late invalid indices, nonzero padding, and a 4,096² image.
  The reviewer independently reran the indexed and binary16 reference scripts.
- The full pinned render suite passed **585 statistical gates and eight
  sensitivity controls**, across **51 fixtures and 1,640 PFM outputs**. The two
  new fixtures contribute 12 gates. The point transparent-white region's largest
  channel difference was 0.01647, within its 0.05321 threshold. The trilinear
  blend region's largest difference was 0.005754, within 0.009664.
- Separate single-seed PBRT controls confirm that compositing the transparent
  white source color to black changes the point region from approximately
  `(1.00005, 1.00063, 0.99272)` to exact black. Replacing trilinear with bilinear
  changes the blend region's green mean from 0.38516 to 0.30227. These controls
  are separate from the full suite's eight existing sensitivity gates.
- The product CLI published an inspected **256×256 PNG at 256 spp**, with only
  those preview overrides staged from `image-indexed-trilinear.pbrt`. Batch size
  seven committed all **16,777,216 samples in 37 batches**.

Independent review has no outstanding correctness findings. Performance and
older GPUs were not measured. Logs, exact probe dumps, and the preview remain
under `build/indexed-png-validation/`. The independent decoder oracle and
sensitivity controls remain under `build/indexed-review/`. Hosted CI is reported
on the PR separately.

| Artifact | SHA-256 |
| --- | --- |
| Release rypath reference executable | `9f378504a039243a2bdf183ebc9eb2ccf4398a559d7100eafc9b973f17d8fab8` |
| Release Metal library | `d9881411cbb683b298873961fce5a7684ed3e9f1c9612a060ba7b0ce6de60897` |
| Release rypath image probe | `48113080cc652121ed250d0d6110ead185e2f599cd0f01993b7af0393997ed72` |
| Pinned PBRT executable | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |
| Pinned PBRT image probe | `f945ba084aa1c2eb40daf89c4a1031209219c64f2df2199e54f93bbdd24aa02a` |
| Full comparison log | `6bcd668bc26de24c097d9475a11ff1ba0b0d083ebb1be49b80a705b980353091` |
| Indexed exact comparison log | `8acb9225e01a5a09332c66655e28e678f8277ea68c18faa1d34051948e0fbaf9` |
| CLI preview PNG | `66f0a76fe133eb574a2c37de4f5ade2a1c2acff28b95c63a35cfd761b533ce58` |
