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
