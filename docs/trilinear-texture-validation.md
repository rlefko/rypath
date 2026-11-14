# PBRT trilinear texture validation

## Filtering contract

Named PNG diffuse textures accept `"string filter" "trilinear"` through the
existing PBRT imagemap parameter. The default remains bilinear. Each texture owns its
filter choice while textures sharing a path, encoding, and wrap mode retain
one mip pyramid. No decoder, mip generation, UV footprint, sampling dimension,
BSDF PDF, or accumulation contract changes.

The implementation follows
[PBRT b0bdf461 MIPMap::Filter](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/mipmap.cpp).
The footprint width is twice the largest absolute mapped UV derivative, and
`lod = level_count - 1 + log2(max(width, 1e-8))`. A footprint at or above the
coarsest LOD returns that mip's sole texel directly, before mapping coordinates
that could overflow. Otherwise, `i = max(0, floor(lod))` selects the lower mip.
When `i == 0`, PBRT returns base-level bilinear filtering, including fractional
LODs below one. For higher levels, trilinear lookup blends the bilinear samples
at `i` and `i+1` with weight `lod-i`.

Both levels are decoded and interpolated in linear RGB before texture scale,
inversion, albedo clamping, and wavelength-dependent spectral reconstruction.
Black wrapping applies to both bilinear samples, including an upper 1×1 mip.
A standalone 1×1 texture with a narrow footprint also retains black-border
attenuation. These cases differ from the direct coarsest-LOD return.

This changes the footprint-dependent reflectance model and therefore path
contribution values. Random dimensions, proposal PDFs, MIS weights, and the
throughput update rules stay fixed. Raw XYZ remains separate from display conversion.
Point and EWA filtering, other image formats, and textured emission remain unsupported.

## Independent checks

Portable lookup tests use explicit mip texels and independently calculated RGB
weights. They cover interior adjacent levels, integer and base-level LODs,
sRGB and linear decoding, repeat/clamp/black wrapping, upper and standalone 1×1
mips, and malformed upper payloads. Parser tests cover default, inherited,
overridden, and mixed filter selections, source diagnostics, shared storage at
the retained-memory limit, and accurate render provenance.

The native probe uses synthetic transfer values and known mip colors to separate
filtering arithmetic from image decoding. It checks 33 result vectors at each
of four sample budgets, including trilinear selection at nonzero descriptor
indices. Bounds and enum failures return nonfinite probe results; the production
host rejects invalid scene descriptors before upload, including a second filter
descriptor that shares an already validated pyramid.

The image fixture manifest records the unchanged PNGSuite input, its existing
license, and the owned trilinear wrapper. Selected asset coverage does not
establish support for additional upstream scene entry points.

## Pinned PBRT comparison

The owned `image-trilinear.pbrt` wrapper reuses the unmodified 32×32 PNGSuite
image recorded in the [image manifest](../tests/fixtures/pbrt-render/image-assets/README.md).
It renders a front-facing triangle pair at 8×8 and 4,096 samples per pixel,
with a small triangle pixel filter. Near the center, mapped footprints select
LOD approximately 2.5, blending 8×8 and 4×4 mips. A two-pixel region straddles
the image's repeated red transition, making adjacent-mip blending observable.

The comparison uses the existing 16 independent seeds per backend (rypath 0–15,
PBRT 16–31), matched scene settings, and linear PFM output. Every region/channel
must satisfy `abs(delta) <= 5 * combined_SE + 2e-5 * max(1, abs(reference))`.
Regions and thresholds were fixed before rendering. The first seed from each
backend also renders a control with only `trilinear` changed to `bilinear`;
its red region mean must increase by more than 0.02.

All **354 statistical gates and both sensitivity gates passed** across 28
fixtures and 898 PFM outputs. The new fixture's largest RGB mean difference
was 0.003514. The same-seed bilinear-minus-trilinear red differences were
0.07387 for rypath and 0.07724 for PBRT, exceeding the declared minimum.
The original 27 fixtures retained their settings and thresholds. This is
numerical agreement for the matched profile, not bitwise CPU/GPU identity.

```sh
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir build/trilinear-validation/reference
```

The product CLI separately rendered the new scene with batches of seven,
committed all 262,144 samples, reported trilinear provenance, and wrote an
inspected PNG. Reference source and executable provenance remain pinned by
the [reference build record](pbrt-render-validation.md#pinned-upstream-reference).

## Implementation validation

Measured on November 14, 2026, for implementation `b7e53e5` based on main
`3406398`: Apple M4 Max (`applegpu_g16s`), macOS 26.6 (25G5065a), Xcode 26.6
(17F113), Metal compiler 32023.883, CMake 4.0.3, and Ninja 1.13.2.

- Debug, Release, and ASan/UBSan: **70/70 CTests each**, Metal enabled with both
  availability allowances disabled. Metal-disabled ASan/UBSan: **58/58**.
- Metal API and shader validation: **14/14** native tests, no skips.
- Same-seed trilinear/RGB controls: maximum absolute path errors **9.27e-6**
  on triangles and **4.45e-6** on patches, below the predeclared 3e-5 tolerance.
  Bilinear and premature-clamping controls differ by more than 0.29 and 0.34.
- Secondary-hit testing changes 409 of 1,414 two-hit paths by more than 0.001;
  one-hit paths remain exactly equal. Both filter modes preserve exact raw films
  across batches of 1, 7, and 65 for 133,120 samples per render. A fixed-budget
  bilinear control verifies the trilinear batch fixture actually blends mips.
- LLVM 18.1.8 clang-tidy passes all seven changed C++ units. Apple Clang Static
  Analyzer passes the changed native adapter and probe. All **27 Python tests**,
  repository hooks, and independent implementation/evidence reviews pass.

The initial narrow-camera triangle test hit a shared edge and missed in both
the textured and control scenes. Translating the fixture into a triangle's
interior, while compensating its UV offset, preserved the intended texture
sample and restored the required one-hit comparison. Independent review also
identified that the original batch fixture selected only the base mip; a larger
mapping and a same-budget filter control now require actual blending. Neither
correction changed production code or loosened thresholds.

The Release reference executable SHA-256 is
`5ef3177a35474ea6a634bd241bcabe59103121a04d3dbe6961dcb9b6746ec79b`;
its Metal library is
`d133c9ac81776189bfb986c689949aeb860b86758e7e821093a473ddc5a45bd4`.
The PBRT executable remains
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.

Run the configure/build/CTest commands for all three supported presets, then:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  ctest --preset debug --no-tests=error --verbose -R '^metal\.'
cmake -S . -B build/portable-image -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DRYPATH_ENABLE_METAL=OFF -DRYPATH_ENABLE_SANITIZERS=ON
cmake --build build/portable-image
ctest --test-dir build/portable-image --no-tests=error --output-on-failure
```

Hosted CI is recorded on the PR separately from strict M4 Max execution.
Performance and broader corpus coverage are outside this validation.
