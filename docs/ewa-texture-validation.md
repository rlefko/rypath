# PBRT EWA texture validation

## Model and compatibility contract

Spectrum PNG diffuse imagemaps accept `"string filter" "ewa"` and `"EWA"`.
Bilinear remains the default. EWA uses `maxanisotropy=8` by default and accepts
finite values in [1,16]. Other finite authored values produce a source-located
unsupported-feature diagnostic, including inherited values and unused definitions.
This is an explicit rypath profile restriction; pinned PBRT's CPU constructor
does not enforce that range. Other filters keep their inert finite parameter.

The semantic reference is the CPU implementation at
[PBRT b0bdf461](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/mipmap.cpp).
Each descriptor carries its filter and anisotropy independently of the shared
image pyramid. EWA uses wire tag 3, preserving earlier tags, and adds a float at
offset 44 for a 48-byte descriptor. Existing image dimensions and byte budgets
remain in force. No camera, geometry, or sampling interface changes are needed.

After UV mapping, order the two derivative vectors by length and widen the
shorter vector when their length ratio exceeds the limit. A zero shorter vector
uses base-level bilinear interpolation. Otherwise, the corrected shorter length
`b` selects `lod=max(0,level_count-1+log2(b))`; blend EWA at the adjacent levels,
including fractional LODs below one. There is no factor of two in this LOD.
The actual 1×1 mip still evaluates the wrapped ellipse. An EWA level beyond the
pyramid returns the top texel directly, so only `b>=2` gives a wholly
coordinate-independent return. The texture flips the vertical center coordinate
without negating the mapped derivative components, matching pinned CPU PBRT.

Use PBRT's exact 128-entry table, indexed by `min(floor(128*r²),127)` for `r²<1`.
Its last entry is zero. Decode texels before weighted RGB averaging. Black taps
outside the image retain their weights in the denominator. Texture scale,
inversion, albedo clamping, and spectral reconstruction follow filtering.
This changes the footprint-dependent reflectance model, with no new random
dimensions, proposal densities, MIS weights, or sample stopping rules.

## Numerical adaptations and bounded work

Lengths and anisotropy widening avoid squaring huge vectors or dividing by a
tiny unscaled shorter length. The exactly-zero shorter-vector case precedes
wide-footprint handling. Metal flushes binary32 subnormals even in the strict
shader configuration. An EWA-only integer multiplication path preserves mapped
products involving subnormals or native underflow, with round-to-nearest-even.
It applies to center mapping as well as derivatives: a tiny scale times a large
finite UV can have a visible normal-sized result. Bitwise nonzero classification
and a common power-of-two lift preserve tiny derivative directions through
anisotropy widening. Existing filter arithmetic is unchanged.

For texel-space axes `u,v` and displacement `d`, use
the nonnegative equivalent covariance formulas:

```text
D = 1 + dot(u,u) + dot(v,v) + cross(u,v)^2
r² = (dot(d,d) + cross(u,d)^2 + cross(v,d)^2) / D
radius_i = sqrt(1 + u_i^2 + v_i^2)
```

These avoid subtracting nearly equal determinants for collinear or slanted
axes. They are numerical adaptations, not promises of bitwise PBRT agreement
at LUT bins, ellipse boundaries, or representable neighbors of LOD transitions.

At every scanned mip, the corrected shorter axis has texel length below two
and the longer axis below twice the anisotropy limit. The sum of squared box
radii is at most `2+4*(16²+1)=1030`; this also holds for rectangular mip chains.
The bounding box has fewer than 2,152 candidate integer texels in exact
arithmetic. A checked 4,096-candidate ceiling leaves rounding margin and bounds
execution before integer conversion or iteration. It never truncates an ellipse.
The nearest lattice point has `r²<=1/2` in exact arithmetic, so normalization
has a positive term even when every addressed texel is black.

Preserve signed `st*resolution-0.5` centers through `|st|<=64`. Outside this
range, repeat uses the signed fractional period before scaling. Clamp uses an
integer-translated center beyond the appropriate edge while preserving its
fractional phase: `-64+fmod(st,1)*resolution-0.5` on the low side and
`resolution+64+fmod(st,1)*resolution-0.5` on the high side. This defined
large-coordinate extension preserves phase where PBRT's float arithmetic can
lose it and avoids eventual unsafe integer conversions.
Near-edge centers are not clamped: a slanted ellipse outside the image can
still overlap interior texels. Black returns zero early only for a mip whose
entire support misses the image; this does not bypass its adjacent mip blend.

Invalid payloads, unrepresentable required arithmetic, excessive scan bounds,
and nonpositive or nonfinite normalization fail visibly. Rendering rejects the
pending batch without committing partial samples or substituting another filter.

## Acceptance criteria

Portable and native lookup vectors use an independent double-precision oracle
with independently generated exponential table values and explicit tap
addressing. Stable vectors retain the existing absolute `2e-6` tolerance.
Cases cover asymmetric rotated patterns, signed/swapped axes, active anisotropy
limits, zero and collinear derivatives, base and terminal mip transitions,
rectangular images, all wrap modes, large coordinates, and malformed payloads.

Production triangle and bilinear-patch controls compare to independently
filtered constant RGB materials at the same sampled wavelengths, using
`abs(delta)<=1e-7+3e-5*white_radiance`. Trilinear substitution and changing the
anisotropy limit must each produce a path difference greater than 0.02.
The secondary-hit control requires more than 100 changed two-hit paths at a
0.001 difference and a maximum difference above 0.01; one-hit paths stay exact.
Batch sizes 65, 7, and 1 must preserve raw films and counts for 133,120 samples,
crossing the native dispatch capacity.

The owned `image-ewa.pbrt` wrapper reuses the unchanged pinned PNGSuite asset.
At 8×8 pixels and 4,096 spp, its full-image and fixed pixel (4,3) means use
16 independent seeds per backend and the existing linear-PFM gate
`abs(delta)<=5*combined_SE+2e-5*max(1,abs(reference))`. First-seed red at that
pixel must decrease by more than 0.02 under both trilinear substitution and
reducing EWA anisotropy from 8 to 4, independently on each backend. These
thresholds and inputs precede acceptance rendering. Asset closure, checksums,
and attribution remain in the fixture manifest. This is selected asset coverage,
not support for an additional unmodified upstream scene entry point.

## Reproduction

Run configure, build, and CTest with each supported `debug`, `release`, and
`asan-ubsan` preset, with both Metal availability allowances disabled. Also run
a separate Metal-disabled sanitizer build. Native validation and the opt-in
pinned comparison use:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --preset debug --no-tests=error -R '^metal\.'
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir build/ewa-texture-validation/reference
```

The reference provenance is retained in the
[pinned build record](pbrt-render-validation.md#pinned-upstream-reference).
Routine CTest uses local fixtures and needs no external downloads.

## Measured evidence

Measured November 14, 2026 for implementation `d71f547`, based on main
`1301d08`, on Apple M4 Max (`applegpu_g16s`), macOS 26.6 (25G5065a),
Xcode 26.6 (17F113), Apple Clang 21, CMake 4.0.3, and Ninja 1.13.2.
Both Metal availability allowances were disabled.

- Debug, Release, and ASan/UBSan each passed **74/74 CTests**. The separate
  Metal-disabled sanitizer build passed **61/61**. Metal API and shader
  validation passed **15/15**, with no skips.
- The native probe checked **103 result vectors at each of four sample
  budgets**, including 39 EWA cases. Independent review found that Metal's
  subnormal flushing changed both minor-axis widening and center mapping.
  The corrected implementation retains direct regression cases for both.
  Splitting the test dispatch and keeping its legacy path on the shared
  standard-filter implementation reduced cold instrumented compilation from
  76.55 seconds to 15.16 seconds on this run. The existing 30-second timeout
  remains. This is validation-tooling evidence, not a renderer speed claim.
- Triangle and bilinear-patch controls each checked **512 EWA paths** against
  the independent RGB material. Release maximum path error was **4.52995e-6**
  for both, within the fixed radiance-scaled bound. Trilinear substitution
  changed a path by **0.889779**; reducing anisotropy changed one by **0.41118**.
  The secondary control changed **678 of 1,414 two-hit paths** by more than
  0.001, with maximum difference **0.82577**. One-hit paths stayed exact.
  Batch sizes 65, 7, and 1 preserved exact raw films and counts for 133,120
  samples in each of four filter modes. The invalid EWA arithmetic control
  rejected its pending batch with zero committed samples.
- Independent API/shader-validated M4 Max probes matched **400,192 mapped
  products bit for bit** against CPU binary32 multiplication. **75,000 native
  lookups** had zero success/failure mismatches; 4,851 correctly rejected
  overflowing required mapped coordinates. One finite case differed by
  **1.20401e-5** at a hard LUT-bin boundary. Portable sanitizer stress covered
  **22,500 EWA lookups**, with no invalid result or sanitizer report. Six
  differed from the double oracle at LUT boundaries, with maximum **0.00213707**.
  These boundary observations are outside the stable-vector tolerance; they
  do not change that tolerance or establish uniform CPU/GPU equality.
- **180,000 legacy-filter lookups** retained the exact binary32 digest of
  base `1301d08`. All 128 table entries matched pinned PBRT exactly.
- The final Release PBRT comparison passed **546 statistical gates and eight
  sensitivity controls**, across **46 fixtures and 1,480 linear PFM outputs**.
  The EWA wrapper's largest regional mean difference was **0.004113**, within
  its fixed gate. EWA-minus-trilinear red was **0.2122** for rypath and **0.2177**
  for PBRT. Cap-8-minus-cap-4 red was **0.07291** and **0.07395**, respectively.
  Existing fixtures retained their settings and thresholds.
- All **55 Python tests**, LLVM 18.1.8 clang-tidy on changed portable units,
  Apple Clang Static Analyzer on changed native units, formatting, repository
  hooks, and independent review passed. No actionable review findings remained.
- The product CLI published an inspected **256×256 PNG at 256 spp**, with those
  preview overrides recorded in its staged input and the unchanged PNG asset
  beside it. Batches of seven committed all **16,777,216 samples** and reported
  EWA filtering.

Full local logs and independent probe sources are retained under
`build/ewa-texture-validation/`, with reviewer sources and logs in
`independent-review/`. Hosted checks are reported on the PR separately.
This is numerical validation, not full-corpus support or a performance study.

SHA-256 artifact identities:

| Artifact | SHA-256 |
| --- | --- |
| Release rypath reference executable | `e9fcf6cbd4e2924adb434ba06440d503a232df1fff0bba45158a66f0b0027972` |
| Release Metal library | `0c7aec0ddb3864abbfedf738021b8155a358fc7f1dbd3dc9d7eedf515b999b0e` |
| Pinned PBRT executable | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |
| Final full comparison log | `9d96a12a53a395f77c1dc3c762fa9bc2380ea5d6434fffabc808a89328e0382f` |
| CLI preview PNG | `02756febe26ef369e16c68b09f431d32626167201128537839fb88f010bc8c57` |
