# Linear HDR image I/O

This slice follows merged core texturing PR #27 (`8aaa003e`). It changes image decoding, texture storage and final image
publication; it does not change the transport estimator, spectral model,
random sample identities or raw compensated XYZ accumulation. Floating input
can express source values that previous byte/normalized image formats could
not retain.

## Format and numerical contract

PBRT semantics remain pinned to
[`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
The [official file format](https://www.pbrt.org/fileformat-v4) defines film
precision and image consumers; the pinned image/mipmap implementation supplies
the source and filtering oracle. OpenEXR's
[technical introduction](https://openexr.com/en/latest/TechnicalIntroduction.html)
defines channel types, windows and compression. Radiance's
[official format record](https://radsite.lbl.gov/radiance/refer/filefmts.pdf)
defines RGBE framing. The supported rypath subset is deliberately narrower.

| Input | Accepted subset |
| --- | --- |
| EXR | Single-part flat scanline or one-level tiled; exactly Y, RGB or RGBA; consistent HALF or FLOAT; unit sampling |
| EXR windows | Nonzero data origins and partial edge tiles; data-window extent becomes the texture; display window remains metadata |
| EXR compression | Classic NONE, RLE, ZIPS, ZIP, PIZ, PXR24, B44, B44A, DWAA and DWAB |
| Radiance HDR | RGBE signature, `32-bit_rle_rgbe`, flat or modern per-channel RLE, `-Y height +X width` |
| Color | Linear sRGB/D65; missing EXR chromaticities accepted, authored values checked with pinned PBRT's relative `<1e-3` rule |

EXR Y replicates to RGB. All retained RGB/alpha samples must be finite. HALF
widens exactly to binary32 for decoding and retains HALF source metadata;
its mips use binary16. EXR FLOAT and RGBE use binary32 decoded samples and mip
storage, including values above 65,504. A binary32 packed texel occupies four
32-bit words for RGB and optional scalar alpha. Native light and normal views
keep source dimensions. No byte/HALF fallback narrows FLOAT image payloads.

Valid authored encoding is inert for EXR/HDR: these samples always remain
linear, following pinned PBRT. Power-of-two mip bases retain signed samples.
NPOT images use PBRT's separable resize and nonnegative floor after resampling;
this can remove negative resized values. Native views bypass that resize.
Filtering, scale and inversion precede the relevant consumer's clamp. Diffuse image spectra clamp RGB to [0,1] for
albedo reconstruction; scalar and bump consumers retain signed values. RGB
reflectance does not composite alpha. True RGBA alpha follows the existing
scalar filter branches. Existing binary emitting-alpha certification remains
separate. Native image lights retain HDR radiance, subject to their checked
spectral reconstruction and scale limits. Nonfinite or unrepresentable mip
results fail instead of clipping.

EXR excludes multipart/deep, mip/rip levels, UINT, mixed types, layered or extra
channels, partial RGB, subsampling, other chromaticities, HTJ2K, LJ2K and ZSTD.
Damaged chunk tables are not reconstructed. HDR excludes XYZE, other orientation
orders, legacy RGBE repeat-marker RLE, mixed flat/RLE rows, nonidentity exposure,
color correction or pixel aspect, non-sRGB primaries and unknown header
assignments. Malformed streams, missing assets, nonfinite samples and resource
exhaustion are errors; recognized excluded features receive unsupported
diagnostics. Neither decoding nor header processing executes external commands.

Input limits remain 16 MiB per file, 64 MiB total expanded inputs, 4,096 pixels
per source/mip axis, 256 MiB aggregate retained decoded data/views/mips/tables,
and 256 MiB scratch per decode or mip build. EXR also caps tile dimensions at
4,096; Core and embedded libdeflate allocations share the scratch owner, including
allocation-header overhead. Decoded floating RGB consumes 12 bytes per pixel,
or 16 with retained alpha, including widened HALF. HDR header text is limited to
64 KiB and each line to 4 KiB. Byte limits can reject an image whose axes fit.
See the [structural profile](pbrt-profile.md#image-map-definitions) for shared
resource accounting and dependency resolution.

## Film output

`--render-pbrt SCENE --output IMAGE.exr` publishes RGB ZIP OpenEXR. Effective
`Film.savefp16` defaults to true and selects HALF; false selects FLOAT.
`--render-demo --output IMAGE.exr` uses FLOAT. PBRT `.png` retains its existing
display encoding and explicitly overrides `savefp16`; the demo retains its
existing PNG path contract for other suffixes.

EXR uses each pixel's compensated XYZ mean and the existing film projection:
unit exposure for the demo, calibrated sensor exposure for PBRT. It applies no
sRGB transfer, display gamut clip, tone mapping or denoising. Negative and
above-one finite values survive. All pixels must have the same positive count;
raw sums, corrections, means, exposure and projected RGB must be finite.
FLOAT overflow and HALF magnitude above 65,504 fail. Output uses the film extent
rather than the 4,096-axis texture input limit, subject to film/codec bounds and
available resources.

The publisher writes a sibling temporary file through positioned callbacks,
checks codec completion and final offset-table writes, checks close, then
renames over the destination. Failure preserves an existing output and removes
the temporary file. Raw film values and sample counts remain unchanged.

The header records sRGB/D65 chromaticities. String attributes record rypath
version, render profile, scene path, sampler, seed, samples per pixel, batch size,
exposure, finite depth and output overrides. Stdout identifies output format,
precision and actual overrides. This metadata is descriptive provenance; it is
not a checkpoint format or a future resume-identity guarantee.

## Independent evidence

Measurements on 2026-11-14 used Apple M4 Max, macOS 26.6, Apple Clang 21 and
a dirty working tree based on `8aaa003e`, not that
commit's unchanged contents. The installed oracle was OpenEXR C++ 3.3.2;
production uses the vendored static OpenEXRCore 3.5.4 subset. The
[dependency record](../third_party/openexr/README.md) identifies exact source
pins, licenses, excluded dependencies and local adaptations. The oracle SDK is
never linked into the product.

| Check | Obtained result |
| --- | --- |
| Independent installed OpenEXR reader | 48/48 known film channel values across demo/PBRT, HALF/FLOAT, compensated means, exposure, signed values, above-one and FLOAT above-65,504 values |
| Output header checks | Data window, all RGB types, ZIP compression, string metadata and sRGB/D65 chromaticities passed |
| Pinned PBRT source/mip probe | 380/380 dimension/channel values across signed HALF, bright FLOAT and RGBE; repeat/clamp and NPOT resampling |
| Six matched HDR render fixtures | 90/90 regional/channel gates across 192 PFM images; largest observed difference/threshold ratio 0.4701 |
| Output/CLI slice | 12/12 targeted native Debug checks and 12/12 Metal-disabled portable checks passed; native CLI rendering executed |
| Availability policy | EXR-aware fake successes/publication failures and invalid `Wrote EXR` availability cases passed |

Source samples must match exactly. Mips use a predeclared
`3e-6 * max(1, abs(reference))` arithmetic tolerance. The render comparison uses
8×8 pixels, 4,096 spp, rypath seeds 0–15 and PBRT seeds 16–31, full-frame plus
four quadrant means, and the unchanged threshold of five combined standard
errors plus `2e-5 * max(1, abs(reference))`. Thresholds were fixed before running.
These checks supply focused evidence, not proof of general scene conformance.

The [six owned wrappers and asset manifest](../tests/fixtures/pbrt-render/hdr-io/README.md)
cover diffuse clamping, a scalar texture graph, NPOT FLOAT mips, RGBE, visible
HDR area emission, a square NPOT equal-area environment and a selected external
RGBA HALF texture. The external asset is OpenEXR's `comp_zip.exr`, revision
`f723c3940fff287c0a26b425b90a8e764823bfd4`, 587×675, 1,180,708 bytes, SHA-256
`b51491f94c30896e038ab13286922d75f36f65a42533a8c9acac72b81b1a84a8`.
It remains cache-only under its retained BSD-3-Clause license. Complete checked
dependency closure is explicitly staged when each scene is relocated.
Selected asset coverage does not establish support for the full OpenEXR corpus.
Signed bump/normal reference scenes were not added in this comparison suite.

Reproduce the independent checks after building the native reference and output
test targets:

```sh
python3 scripts/compare_hdr_io.py --build build/debug \
  --pbrt-build .cache/pbrt-reference/build \
  --output-dir .cache/hdr-io-reference
```

Use `--skip-renders` for deterministic readback/source/mip checks. The driver
verifies the pinned PBRT source revision and records tool versions and binary
hashes. Local render artifacts are in
`.cache/hdr-io-reference/hdr-io-c0geq8t2`; final strengthened deterministic
readback and provenance are in `hdr-io-b6r6z9xc` under the same cache.
Historical core-texturing build counts and performance measurements remain in
[their own record](core-texture-validation.md).

## Implementation validation

The completed implementation is `8a6ff53`, following the dependency commit
`43c8870`. Debug, Release and ASan/UBSan each pass all **87 tests** on the M4 Max;
the Metal-disabled portable build passes **68/68**. All **71** Python policy and
comparison tests pass. LLVM 18 Clang-Tidy passes all **22 changed C++ units**;
Apple Clang's analyzer passes the changed renderer adapter and new Objective-C++
HDR probe. Formatting and repository policy hooks pass. Hosted checks are
reported on the PR separately.

The native HDR probe compares **2,304** HALF/FLOAT queries across native and mip
views, one/three/four source channels, all four filters, all wraps and varied
footprints. It includes signed samples, HALF subnormals and FLOAT above 65,504.
The integrated render regression checks exact sample identity across batch and
threadgroup partitions with HDR lights and a FLOAT normal map. Invalid zero
normals and scalar overflow fail the whole batch with no committed samples.
CPU PLY displacement rejects the same scalar overflow at its source location.

Independent review exercised all **63,488** finite HALF bit patterns against
`_Float16`, **6,000** seeded DWAA/PIZ byte mutations under ASan/UBSan, and **44,120**
scratch-budget cases across DWAA, DWAB, PXR24, B44A and ZIPS. Review found and
closed an allocation-error classification gap and scalar inversion masking NaN;
regressions cover both. Codec tests also cover all ten classic codecs against
independently generated reference samples, truncations, nested and concurrent
allocation scopes, limits, and output finalization failures.

Final deterministic readback on this implementation again passes 48 film
channels and 380 PBRT source/mip values; artifacts and binary hashes are in
`.cache/hdr-io-final/hdr-io-t5exlj8v`. The final six-scene comparison also passes all 90 gates across 192 PFM outputs;
its artifacts are `.cache/hdr-io-final/hdr-io-tms7ij79`. All **21/21** Metal tests
pass with API validation alone (`MTL_DEBUG_LAYER=1`), recorded in
`build/debug/hdr-metal-api-only.log`. Shader instrumentation is recorded
separately below because it uses Apple's runtime compiler service.


With `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`, **20/21** Metal tests pass,
including all 2,304 dedicated HDR queries. `metal.core_texture_render` reaches
its 180-second timeout while Apple's `MTLCompilerService` is active. An earlier
instrumented attempt reported `XPC_ERROR_CONNECTION_INTERRUPTED` during
`rypath_render_paths` specialization and a separate cold footprint-probe timeout;
the footprint probe passes on the isolated run. There is no completed shader
instrumentation result for the combined texture test. This is recorded as a
validation limitation, independently of its passing ordinary GPU regressions.
Logs are `build/debug/hdr-metal-validation.log` and `hdr-metal-final.log`.
