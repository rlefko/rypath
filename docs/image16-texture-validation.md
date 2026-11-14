# PBRT 16-bit PNG texture validation

## Model and compatibility contract

Diffuse spectrum imagemaps accept noninterlaced 16-bit grayscale, grayscale-alpha,
RGB, and RGBA PNGs alongside the existing eight-bit profile. Grayscale replicates
into RGB; alpha is discarded without compositing. PNG metadata does not select
the transfer function. `sRGB` remains the default, with explicit `linear` support.
Point, bilinear, trilinear, and bounded EWA filters retain their existing mapping,
footprint, wrap, scale, inversion, and spectral reconstruction contracts.

The numerical reference is CPU/float
[PBRT b0bdf461](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/image.cpp).
Interpret each source channel as an MSB-first unsigned integer, divide by
`65535.f`, apply the selected transfer, and round to binary16 with ties to even.
The continuous sRGB approximation uses the pinned rational polynomial and fused
multiply-add evaluation; substituting the eight-bit table or a different transfer
formula can change values at half-rounding boundaries.

Before mip generation, widen those source halves to float. Non-power-of-two
sources use the existing separable resampling kernel. Downsample float working
levels and round each stored level independently to binary16: stored rounded
mips are not the input to later reductions. Resampling clamps below zero but
retains overshoot above one. Texture scaling and inversion precede the existing
reflectance clamp. Half subnormals widen through integer bit construction in both
backends, avoiding half arithmetic that could erase dark values.

This changes texture input precision and its declared reflectance model. It adds
no random dimensions, proposal densities, MIS weights, or sample termination.
Raw XYZ accumulation and display PNG output are independent of texture storage.
The film `savefp16` override continues to describe output, not texture mip data.

Pinned PBRT accepts plain grayscale16, RGB16, and RGBA16 inputs. Its bundled
decoder rejects grayscale-alpha16 and grayscale16 with `tRNS` during requested
conversion to GREY16. rypath's RGB16 conversion supports these as explicit profile
extensions. Owned fixtures establish their semantics; they are not counted as
successful pinned reference inputs. Indexed, interlaced, animated, and other
bit-depth PNG inputs remain unsupported.

## Storage and failure behavior

Decoded sources retain six RGB bytes per pixel, shared across encoding and wrap
variants. Image pyramids retain two 32-bit words per RGB half texel: R and G in
the first word, B in the low half of the second. Its unused upper half is zero.
The existing 48-byte Metal descriptor carries the sRGB8, linear8, or linear-half
format. Buffer offsets and lengths count words. Encoded eight-bit pyramids retain
their original one-word layout and arithmetic.

Portable lookup, render validation, upload rebasing, and shader lookup check
format-dependent bounds. Invalid data must fail before a read or prevent the
pending render batch from committing; it must not substitute a different format.
Source-depth-dependent raw, scanline, conversion, and copy buffers contribute to
the existing scratch and retained limits. The 4,096-pixel axis limit is a ceiling,
not a promise every image fits: a full 4,096×4,096 RGB16 source and its two-word
pyramid exceed the combined 256 MiB retained budget.

The PNG decoder also caps each allocation at 128 MiB. Its bounded Huffman-buffer
growth can reach this cap even when the expected scanline data and retained
pyramid fit their separate budgets. Allocation refusal is a `limit-exceeded`
diagnostic rather than evidence that the image is corrupt.

Source conversion matches the pinned reference exactly over the complete 16-bit
domain tested below. Non-power-of-two resizing retains the existing rypath float
arithmetic, whose compiler may contract multiply-add operations. The pinned CPU
reference disables contraction. Half rounding can therefore differ near a boundary;
the exact comparisons below establish selected mip cases, not universal bitwise
identity for resized images. The existing eight-bit path and comparison thresholds
are preserved.

## Acceptance criteria

- Exhaust all 65,536 input channel values in each supported encoding against the
  pinned source and half-storage path. Compare selected mip bits exactly, including
  the linear source `[1, 1, 255, 4661]`: its coarsest half must be `0x24cd`.
  Skipping source rounding or propagating rounded intermediate mips gives `0x24ce`.
- Check source byte order and low-byte sensitivity, grayscale and alpha behavior,
  malformed payloads, exact retained-budget boundaries, and shared source/pyramid
  lifetimes. Check non-power-of-two overshoot before and after scale below one.
- Native probes must preserve dark half subnormals exactly. Stable filtered
  vectors retain the existing numerical tolerance; generic lookup tolerances
  must not stand in for exact low-value checks. Mixed encoded8/half buffers must
  exercise nonzero offsets, selected and blended levels, and coarsest shortcuts.
- Production controls exercise triangle and bilinear-patch hits, secondary
  transport, invalid batches, and exact progressive sample identities and counts.
- Independent PBRT images retain the existing regional gate:
  `abs(delta) <= 5*combined_SE + 2e-5*max(1, abs(reference))`, using 16 fixed
  independent seeds per backend, 8×8 output, and 4,096 spp. Deterministic precision
  probes carry the low-byte and rounding evidence that Monte Carlo noise can hide.

Selected external coverage uses unmodified pinned PNGSuite RGB16 and grayscale16
images in owned wrappers. The fixture manifest retains the source revision,
hashes, dependencies, settings, and original terms. This does not establish support
for an additional unmodified upstream scene entry point or full-corpus coverage.

## Reproduction

Configure, build, and test the `debug`, `release`, and `asan-ubsan` presets with
Metal availability allowances disabled. Also run a Metal-disabled sanitizer build.

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --preset debug --no-tests=error -R '^metal\.'
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir build/image16-texture-validation/reference
```

The cached reference build and its dependencies follow the
[pinned build record](pbrt-render-validation.md#pinned-upstream-reference).
Routine CTest remains offline. Extended reference validation is opt-in.

Build the separate pinned PBRT image probe against the cached CPU/float build.
This macOS command uses the same OpenEXR installation as that reference build:

```sh
pbrt_source="$PWD/.cache/pbrt-reference/source"
pbrt_build="$PWD/.cache/pbrt-reference/build"
validation_dir=build/image16-texture-validation
mkdir -p "$validation_dir"
c++ -std=c++17 -ffp-contract=off -I"$pbrt_source/src" -I"$pbrt_source/src/ext" -I"$pbrt_build" \
  scripts/pbrt_image16_probe.cpp "$pbrt_build/libpbrt_lib.a" \
  "$pbrt_build/src/ext/libdeflate/libdeflate.a" \
  "$pbrt_build/src/ext/double-conversion/libdouble-conversion.a" \
  "$pbrt_build/src/ext/utf8proc/libutf8proc.a" \
  "$pbrt_build/src/ext/ptex/src/ptex/libPtex.a" \
  "$pbrt_build/src/ext/glfw/src/libglfw3.a" \
  "$pbrt_build/src/ext/glad/libglad.a" \
  $(pkg-config --libs OpenEXR) -lz -framework OpenGL -framework Cocoa \
  -framework IOKit -framework CoreFoundation -o "$validation_dir/pbrt_image16_probe"
python3 scripts/compare_pbrt_image16.py "$pbrt_source" \
  "$validation_dir/pbrt_image16_probe" build/release/tests/rypath_image16_probe \
  tests/fixtures/pbrt-render/image-assets --output-directory "$validation_dir/exact"
```

## Measured evidence

Measured November 14, 2026 for implementation `28bffc7`, based on main
`dff9c79`, on Apple M4 Max (`applegpu_g16s`), macOS 26.6 (25G5065a),
Xcode 26.6 (17F113), Apple Clang 21, CMake 4.0.3, and Ninja 1.13.2.
Both Metal availability allowances were disabled.

- Debug, Release, and ASan/UBSan each passed **74/74 CTests**. The separate
  Metal-disabled sanitizer build passed **61/61**. Metal API and shader validation
  passed **15/15**, without skips. All **61 Python tests**, LLVM 18 clang-tidy,
  Apple Clang Static Analyzer, formatting, and repository hooks passed.
- The pinned probe matched **540,882 stored RGB half values**. Each encoding
  covers all 65,536 source values in every channel. Independent Python binary16
  rounding checks the linear base and selected reductions. Source-half-before-resize,
  overshoot, alpha, authored low bytes, and external asset cases also passed.
- Native deterministic tests cover **39 new lookup cases at four sample budgets**.
  Scaled overshoot controls checked 128 paths per filter on both triangles and
  bilinear patches with zero maximum path error. Premature clipping changes a path
  by **0.0562271**. Mixed encoded8/half secondary transport agreed within
  **5.96046e-8**, including 1,414 two-hit paths. Batch sizes 65, 7, and 1 retained
  exact raw films and counts for 133,120 samples in every filter and storage mode.
- Independent API/shader-validated probes checked **all 65,536 binary16 patterns**
  and **393,304 production-header lookups**, including malformed formats, dimensions,
  offsets, and missing second words. Separate production controls matched exactly
  over **8,448 paths**, including 5,656 two-hit paths and shared half storage before
  an encoded8 pyramid. A failed lane published no partial batch; retry recovered the
  valid lane exactly, and progressive accumulation retained its two prior samples.
- Portable review checked **95,232 widened channels** against an independent
  binary16 oracle. All **8,075,112 encoded8 mip words** across 384 cases matched base
  `dff9c79` exactly. A larger NPOT study compared **24,225,420 half values** across
  388 cases and observed 1,471 differences, each one half ULP. Maximum absolute
  difference was **0.00048828125**. Maximum relative difference was 25% between
  subnormal codes 4 and 5, an absolute **5.960464477539063e-8**; no zero-code
  mismatches occurred. Disabling contraction in an isolated diagnostic build
  eliminated every difference. These are scoped observations, not a universal error
  bound. Production retains the established arithmetic and acceptance thresholds.
- Review found the valid 3,072×4,096 RGBA16 allocation-cap case was incorrectly
  labeled corrupt. The corrected source-located `limit-exceeded` diagnostic and
  independent fixed-Huffman regression passed; the allocation cap remains unchanged.
- The final pinned render comparison passed **573 statistical gates and eight
  sensitivity controls**, across **49 fixtures and 1,576 PFM outputs**. The three new
  fixtures contribute 27 gates. Their maximum regional differences were 0.002241
  (authored low bytes), 0.003606 (RGB16 trilinear), and 0.002220 (grayscale16 EWA),
  within the predeclared gates.
- The product CLI published an inspected **256×256 PNG at 256 spp**, with only those
  preview overrides staged from `image16-magnified.pbrt`. Batch size seven committed
  all **16,777,216 samples** in 37 batches.

Independent review has no outstanding correctness findings. Older GPUs and
performance at maximum image sizes were not measured. The full logs, exact dumps,
preview, and independent reviewer sources are retained under
`build/image16-texture-validation/`. Hosted checks are reported on the PR separately.

| Artifact | SHA-256 |
| --- | --- |
| Release rypath reference executable | `386b7c678a689b32c70ab8f51192c00670346825311f725f3b3733f7294d92d4` |
| Release Metal library | `d9881411cbb683b298873961fce5a7684ed3e9f1c9612a060ba7b0ce6de60897` |
| Pinned PBRT executable | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |
| Pinned PBRT image probe | `130249ed38a9530ca062012a3975a8a0d0f0ef0ef3916de9c8179e7935a75394` |
| Final full comparison log | `50995e84e05bcc7358f6fbad35a8d468e466ce602a920481beff0e281bbf0ee0` |
| Exact mip comparison log | `b2144631d415ce5db4779ca57315332a637fd2a0e5410e5fef27661487ac3397` |
| CLI preview PNG | `f573a8b42a3409a7929dcc463b80c9632d13a801ec81da60f40a7d311dce15a2` |
