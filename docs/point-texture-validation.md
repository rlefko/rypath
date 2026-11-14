# PBRT point texture validation

## Model and numerical contract

The spectrum PNG diffuse imagemap profile accepts `"string filter" "point"`.
Bilinear remains the default. Filter choice belongs to each texture descriptor;
textures with the same image, encoding, and wrap share the existing mip pyramid.
The wire tag is 2, preserving prior tags and the 44-byte descriptor layout.

The semantic reference is the CPU implementation at
[PBRT b0bdf461](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/mipmap.cpp#L229-L246).
Footprint width remains twice the largest absolute mapped UV derivative and
`lod = level_count - 1 + log2(max(width, 1e-8))`. At or above the coarsest LOD,
lookup returns that mip's sole texel before coordinate mapping. Otherwise,
`max(0, floor(lod))` selects one mip, with no interpolation between levels.

After UV mapping and vertical flip, each axis uses
`round(st * resolution - 0.5)`, with halfway values rounded away from zero.
Wrap applies to that integer index. At width 4, repeat lookup at `s=-0.25`
selects texel 2; normalizing the coordinate first would select texel 3.
Repeat at `s=0` selects the last column, whereas `s=1` selects the first.
Black addressing returns black at both exact endpoints. A standalone 1×1
image retains these narrow-footprint edges; the wide-footprint return bypasses them.

Point mapping keeps UV multiplication and offset addition separate, matching
the pinned CPU build's disabled FP contraction. For binary32
`u=nextafter(1,+infinity)`, `uscale=nextafter(1,0)`, and `udelta=-1`, separate
operations produce zero, while contraction produces a small positive value
that changes the selected texel. Other filters retain their existing mapping.
Point LOD likewise separates logarithm scaling from the level-count addition:
at seven levels and width `0x1.fffffap-2`, contraction selects mip 4 on the
measured device while separate operations select mip 5, matching the
uncontracted CPU calculation. CPU and Metal logarithms can still differ at immediate representable
neighbors of mip transitions. This numerical boundary is not a promise of
bitwise cross-backend LOD agreement; direct regressions isolate the operation
order, and image-comparison regions stay clear of these discontinuities.

For repeat, reduce the rounded float index modulo the dimension before integer
conversion. The finite remainder is a bounded exact integer. If scaling a finite
mapped coordinate overflows, select repeat index zero: supported power-of-two
mips at those binary32 magnitudes span whole periods, with the half-texel offset
below their precision. This is a deterministic numerical extension beyond
PBRT's defined integer-conversion domain. Clamp and black addressing bound
coordinates before scaling while preserving edge selection. Narrow lookups
whose UV mapping becomes nonfinite still fail visibly. Wide footprints retain
the existing coordinate-independent early return.

Decode the selected texel to linear RGB, apply scale and inversion, clamp the
albedo, and reconstruct the spectrum as before. Point filtering changes the
footprint-dependent reflectance model. It does not change random dimensions,
proposal PDFs, MIS weights, throughput rules, or raw sample accounting.
EWA, other image formats, and textured emission remain outside this profile.

## Acceptance criteria

Portable and native lookup checks use distinct authored texels to identify
selected indices independently. Cases cover signed ties and their neighbors,
all wrap modes, fractional LODs, narrow and wide 1×1 mips, extreme finite
coordinates, separate mapping operations, descriptor indices, and invalid
payloads. Native numerical vectors retain the existing absolute `2e-6` bound;
selected point texel identities use exact checks. Production render controls
also retain the existing output-guard verification.

Same-seed triangle and bilinear-patch controls use a known selected sRGB texel,
independent transfer decoding, texture scale/inversion, and a constant RGB
material. Per-path differences must satisfy
`abs(delta) <= 1e-7 + 3e-5 * white_radiance`; bilinear substitution must change
at least one path by more than `0.02`. Secondary-hit controls require more than
100 affected two-hit paths and a maximum difference over `0.01`, while paths
without a secondary hit remain identical. Batch sizes 65, 7, and 1 must preserve
exact raw films and all sample counts, including dispatch partitioning.

The owned `image-point.pbrt` wrapper reuses the unchanged pinned PNGSuite asset.
Its fixed 8×8 image and 4,096 spp use a fractional LOD near 2.625 and a narrow
triangle pixel filter. Comparison regions avoid texel discontinuities. The full
image and column 4, rows 3–4 use 16 independent seeds per backend and the existing
linear PFM gate `abs(delta) <= 5*combined_SE + 2e-5*max(1,abs(reference))`.
At the first seed per backend, point-minus-bilinear red in the two-pixel region
must exceed `0.02`. Settings, regions, and thresholds were fixed before rendering.
The fixture manifest records dependency closure, hashes, and attribution;
this is selected asset coverage, not an additional upstream scene entry point.

## Measured evidence

Measured November 14, 2026 for implementation `6490115`, based on main
`04554f3`, on Apple M4 Max (`applegpu_g16s`), macOS 26.6 (25G5065a),
Xcode 26.6 (17F113), Apple Clang 21, CMake 4.0.3, and Ninja 1.13.2.
Both Metal availability allowances were disabled.

- Debug, Release, and ASan/UBSan each passed **74/74 CTests**. The
  Metal-disabled sanitizer build passed **61/61**. Metal API and shader
  validation passed **15/15**, without skips.
- The native lookup probe passed **64 result vectors at each of four sample
  budgets**, including 31 new point cases. The rounding regression selected
  mip 5 with separate operations and mip 4 with contraction on this M4 Max.
- Triangle and bilinear-patch controls each checked **8,192 point paths**.
  Maximum observed radiance difference from the RGB control was zero on both;
  bilinear substitution changed radiance by up to **0.928764**. The secondary
  control changed **602 of 1,414 two-hit paths** by more than 0.001, with maximum
  difference **1.00901**. One-hit paths were unchanged. Batch sizes 65, 7, and 1
  preserved exact raw films and all counts for the point, bilinear, and trilinear
  scenes, each with 133,120 samples.
- Independent review ran **65,289 repeat-coordinate checks** against a separate
  binary32-rounding/double-remainder oracle and **6,600 LOD arithmetic checks**
  against independently rounded device logarithms, with zero failures.
  The review found the avoidable LOD contraction difference; both backends were
  corrected and covered by the committed regression tests. No actionable review
  findings remained.
- The full pinned PBRT image comparison passed **540 statistical gates and four
  sensitivity gates**, across **45 fixtures and 1,444 PFM outputs**. The point
  wrapper's largest regional RGB mean difference was **0.00509**, within the
  fixed statistical bound. Its point-minus-bilinear red differences were
  **0.2666** for rypath and **0.2679** for PBRT, above the predefined 0.02 floor.
  Existing fixtures retained their settings and thresholds.
- All **53 Python tests**, LLVM 18.1.8 clang-tidy on changed portable units,
  Apple Clang Static Analyzer on changed native units, formatting, and repository
  hooks passed. Comparator tests reject missing/duplicate filter controls and
  insufficient, reversed, or threshold-equal sensitivity differences.
- The product CLI published an inspected **256×256 PNG at 256 spp**, with those
  preview overrides recorded in its staged input and the unchanged PNG dependency
  beside it. Batches of seven completed all **16,777,216 samples** and reported
  point filtering.

Hosted checks are reported on the PR separately. This is numerical validation,
not a performance study or evidence of full PBRT corpus coverage.

## Reproduction

Use the matching configure/build/CTest presets for `debug`, `release`, and
`asan-ubsan`, with both availability allowances disabled. A separate portable
sanitizer build uses `cmake --preset asan-ubsan -B build/point-portable
-DRYPATH_ENABLE_METAL=OFF`, followed by its build and CTest commands.

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --preset debug --no-tests=error -R '^metal\.'
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir build/point-texture-validation/reference
```

The pinned CPU build and dependencies retain the provenance in the
[reference build record](pbrt-render-validation.md#pinned-upstream-reference).
The extended comparison stays opt-in; routine CTest needs no external checkout
or new downloaded assets. Independent reviewer probes and logs were retained
locally under `build/point-texture-validation/independent-{repeat,lod}.{swift,log}`;
the committed portable/native regressions cover the resolved rounding issues.

SHA-256 artifact identities:

| Artifact | SHA-256 |
| --- | --- |
| Release rypath reference executable | `198b58619f754be4f5dfcfae486ca1a4b317dba9467653c983f86ee402596abb` |
| Release Metal library | `23206f51ac7440e3962a0d8e552d3000ffadce025dff003298e185526301bc25` |
| Pinned PBRT executable | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |
| Full image comparison log | `217e8f8a63c5eb3953eadfc4bd9b47a1647cd957bd8a77d4cc0c12d32edb2b23` |
| Independent repeat probe | `f0ebcfabfb6dd81396755bf39ba58a208aab7be2d5ddc29f0b24336c1e8aae0a` |
| Independent LOD probe | `6ee428f15bcfca9e75467e1f5d5ca4cad12301c2aad5dbcb5a30168bca9a4986` |
| CLI preview PNG | `05152ba725fcde78c3b6435a1e2aeb473c73e6901bf935f6692c235a853601dd` |
