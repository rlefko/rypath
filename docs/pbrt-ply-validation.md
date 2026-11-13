# PLY and bilinear rendering validation

This change adds bounded PLY ingestion and renders authored quads as bilinear
patches, including nonplanar surfaces, interpolated normals, retained UVs and
constant diffuse emission. It does not tessellate quads. Raw spectral accumulation,
film projection, the existing triangle transport path and sample dimensions remain
separate from display output.

## Compatibility and input closure

The [preflight profile](pbrt-profile.md#ply-meshes) specifies PLY 1.0 ASCII and both
binary byte orders, scalar types, triangle/quad topology, property selection,
resource limits and diagnostics. A small original decoder was selected after
reviewing RPly, tinyply and happly: their existing allocation/read paths did not
provide this profile's bounds before consuming untrusted declarations. No new
runtime dependency is added; [dependency notes](dependencies.md) record the choice.

The pinned semantic reference is [PBRT b0bdf461](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
The portable tests cover all eight scalar types and both byte orders, ignored
properties/lists, UV alias priority, normals, quads, malformed headers and payloads,
nonfinite/range failures, unsupported larger faces, source locations, Includes,
object/material/orientation state, typed alpha/displacement references, dependency
caching and limits before allocations. ASCII decimal underflow to zero is a
reported restriction; hex floating literals are rejected consistently. Geometry
copies and expanded placement validation have separate budgets.

Three owned PBRT wrappers exercise a mixed triangle/nonplanar quad, a curved
emitter and a transformed external quad. The external file is the unchanged
412-byte `Plane_0000_m000.ply` from the pinned Sportscar model by Yasutoshi Mori,
under CC BY 2.0. The [asset manifest](../tests/fixtures/pbrt-render/ply-assets/README.md)
records its source revision, license, hash, size and complete dependency closure.
This establishes selected asset coverage, not support for the original Sportscar
scene. Configure, build and CTest stay offline; large scene assets remain outside Git.

## Surface and intersection contract

A cyclic quad becomes tensor corners `(p00,p10,p01,p11)=(0,1,3,2)`:

```
P(u,v) = (1-u)(1-v)p00 + u(1-v)p10 + (1-u)v p01 + uv p11
J(u,v) = |P_u cross P_v|,  0 <= u,v <= 1
```

The derivative cross product is affine on the square. A double-precision
support-plane certificate bounds its norm over the entire domain, with an
explicit binary32 arithmetic margin; checking only corners would miss interior
singularities. Folded, singular or uncertified patches are rejected. This is a
conservative regularity domain, not an exact classification of every quad.
Render validation encloses the shader's actual local-edge interpolation and
matrix products relative to the certified world-corner tangents. It rejects
cancellation that could erase a tangent. It separately requires every world
coordinate ULP and the projected position/instance-traversal offset budget to
stay below 1/1024 of the certified minimum tangent altitude. These conservative
conditioning restrictions can reject a regular surface. They do not establish
an arbitrary minimum separation from another primitive; closely spaced geometry,
extreme transforms and pair underflow remain outside the validated domain.
One-sided attributed emitters require a consistent sign of `N dot (P_u cross P_v)`:
nine degree-(2,2) Bernstein coefficients certify the whole square. Two-sided
emission does not need that orientation restriction.

Native triangles and patch bounding boxes use separate homogeneous child
acceleration structures, because Metal rejects a mixed descriptor list within
one child. A mixed source placement expands to two native instances with stable
source mapping. Patch corners live in per-primitive data, and each compute
pipeline owns its intersection function table. The deployment minimum stays
macOS 14 / MSL 3.1. All allocation, build and command failures remain diagnostics.

The patch intersection function projects perpendicular to the dominant ray
axis, chooses the better-conditioned ruling family, and solves the quadratic
with compensated float pairs. It checks both roots, the original closed unit
square, the original ray interval and the projected residual. A symmetric
projected line-segment test handles shared straight edges directly, avoiding
quadratic-root drift beyond the unit interval; it brackets endpoints and checks
compensated collinearity without enlarging the patch domain. A nearest accepted
payload carries distance, source candidate and UV together, including exact ties.
It avoids reconstructing UV through a second, differently rounded inverse
transform. Metal traversal still supplies triangle intersections and any-hit
visibility. Exactly coplanar rulings with no isolated root are misses.

This follows the bilinear surface/ruling formulation in [PBRT, Bilinear Patches](https://pbr-book.org/4ed/Shapes/Bilinear_Patches),
with an adaptation for MSL's lack of double precision. [Source attribution](../third_party/pbrt/README.md)
retains the Apache license. The [Apple per-primitive data documentation](https://developer.apple.com/documentation/metal/improving-ray-tracing-data-access-using-per-primitive-data)
and [ray-tracing intersection/payload guidance](https://developer.apple.com/videos/play/wwdc2020/10012/)
cover the native mechanism. Compensated arithmetic is not an exact predicate:
extreme exponents, pair underflow and arbitrarily ill-conditioned projected
constraints remain outside the validated domain.

## Estimator contract

Triangle and patch emitters share `M=bit_ceil(N)` slots: each real primitive has
probability `1/M`, and unused slots contribute zero. A patch draws uniform
parameter coordinates from a 23-bit midpoint square, with area density `1/J`.
Its solid-angle density at endpoint `y` from `x` is
`|y-x|^2 / (M J(y) |n_g(y) dot direction|)`. Both NEE and BSDF-hit MIS use this
same endpoint density. The geometric Jacobian and cosine are distinct from the
interpolated shading normal used by the diffuse BSDF. Exact tangency contributes
zero to NEE; a tangent BSDF hit retains full weight.

A curved primitive may illuminate itself. The triangle-only same-primitive
shortcut therefore excludes patches; visibility can reject an occluded second
root of the same patch. Multiple endpoint proposals remain valid because hidden
endpoints contribute zero. Patch positions and committed ray points retain float-pair residuals. Ray offsets
add a separate allowance for Metal's inverse instance transform and the
reconstruction discrepancy. Physical endpoints determine the patch light PDF;
only visibility rays use displaced endpoints. Coincident represented samples of
the same patch contribute zero while preserving BSDF continuation. These bounds
do not reuse a triangle barycentric-error allowance.

## Acceptance criteria fixed before measurement

`geometry.bilinear` covers portable regularity and affine cases. `metal.bilinear`
checks 16 analytic rays plus 4,099 deterministic saddle rays against an independent
double oracle for `z=x*y`, using both closest-hit and occlusion queries. Hit/miss,
unique IDs and visibility must match; analytic distance tolerance is
`32 FLT_EPSILON max(1,|t|)`, UV tolerance `64 FLT_EPSILON`. Three additional rays
exercise co-nearest parameterizations and instances, a closer triangle and a
non-dyadic affine transform; that transform has a separately declared
`256 FLT_EPSILON` tolerance. Equal-depth candidates must return their own UV tuple. Another 8,193 exact dyadic
rays exercise shared edges/vertices with opposite endpoint order, plus six
one-ULP outer-boundary cases. Affine and recentered furnace tests detect native
self-intersection; paired translated lighting has a fixed 1% mean-drift budget.
A single absolute sample reproduces the coincident planar self-sample case.

`render.patch_reference` derives mean and second moment with independent
Gauss-Legendre quadrature. Different full quadrature orders must agree within
`2e-7`. `metal.patch_render` uses 32,749 paths for each of two fixed seeds and
compares the scalar transport mean against six analytic standard errors plus
`3e-5`. Cases include a curved emitter mixed with triangle and null slots, a
receiver with unequal normal lengths, visible same-patch reflection, and a
unit-radiance furnace with reflectance 0.625. Blockers, sidedness and batch
partition equality have exact assertions. These checks provide numerical evidence,
not a proof of unbiasedness or a performance claim.

The pinned CPU PBRT comparison retains all existing fixtures, seeds and tolerances:
8×8, 4,096 spp, sixteen independent seeds per renderer (rypath 0–15, PBRT 16–31),
linear binary32 RGB before PNG encoding, and
`5 combined_SE + 2e-5 max(1,|reference|)` per channel/region. Three new fixtures add
45 full-frame/quadrant gates. The comparison script verifies dependency sizes and
hashes before staging the assets under each relocated entry directory; a unit
test checks both successful staging and tamper rejection.

## Reproduction

Run configure, build and test for `debug`, `release` and `asan-ubsan` with both
Metal availability allowances disabled. Then run:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --preset debug \
  --no-tests=error --verbose -R '^metal\.'
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir .cache/pbrt-reference/ply-final
```

The opt-in PBRT build uses the existing pinned source/submodules and CPU
Release/float configuration described in [render validation](pbrt-render-validation.md).
Its binary SHA-256 is
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.


## Measured evidence

Hardware: Apple M4 Max, 40 GPU cores, `applegpu_g16s`, macOS 26.6 (25G5065a),
Xcode 26.6 (17F113), CMake 4.0.3 and Ninja 1.13.2.

| Check | Result |
| --- | --- |
| Strict M4 Max Debug, Release, ASan/UBSan | 64/64 CTests each, no skips |
| Metal API and shader validation | 12/12 native CTests, no skips |
| Metal-disabled ASan/UBSan | 54/54 CTests |
| LLVM 18 clang-tidy | 37 native and 40 portable C++ units passed |
| Apple static analyzer | Six Objective-C++ units passed |
| Python policy/comparison tests | 25/25 passed |
| Pinned CPU PBRT | 267/267 regional/channel gates, 672 PFM outputs |

In addition to the permanent cases, independent review stress-tested 300,000
saddle rays and 32,768 exact shared-edge rays with zero closest-hit or occlusion
disagreements on M4 Max. Maximum absolute saddle errors were `2.99e-8` in distance
and `3.03e-8` in UV. This stress evidence supplements the bounded routine tests;
it does not establish exact intersection behavior over all floating-point inputs.

| Transport case | Independent mean | Observed means, two fixed seeds | Acceptance half-width |
| --- | ---: | --- | ---: |
| Mixed curved emitter | 0.17711943 | 0.17661240, 0.17791722 | 0.00617604 |
| Unequal receiver normals | 0.20221187 | 0.20341677, 0.20245435 | 0.00722166 |
| Same-patch reflected light, excluding primary 1 | 0.07698254 | 0.07697075, 0.07752001 | 0.00308353 |
| Affine patch furnace | 0.625 | 0.62516907, 0.62630487 | 0.00528334 |

Blocker, emission sidedness, coincident connection and batch assertions passed.
The recentered furnace matches its untranslated independent probe; larger
ill-conditioned translations fail before GPU execution. The comparison suite's
maximum error/threshold ratio is 0.626 in the existing RGB multibounce fixture;
the new PLY maximum is 0.393 (curved emitter, top-left red).

| PLY wrapper | rypath full-frame linear RGB | PBRT full-frame linear RGB |
| --- | --- | --- |
| External quad | (0.65801096, 0.78567260, 0.87089632) | (0.65777076, 0.78619416, 0.87211032) |
| Mixed primitives | (0.75824236, 0.63833223, 0.87844033) | (0.75849794, 0.63809023, 0.87942396) |
| Curved emitter | (0.10979363, 0.095549566, 0.042020103) | (0.10980357, 0.095759441, 0.042111305) |

Product PNGs were rendered and visually inspected using copies of the three
wrappers with only resolution changed to 128×128, spp to 512 and output paths
overridden. Dependencies were hash-checked and staged beside each copy. The
mixed surface, narrow transformed external quad and lit receiver were visible;
scalar spectral noise remains, with no denoising applied. Each committed all
8,388,608 samples. These PNGs are local review artifacts, not reference gates.

| PNG | SHA-256 |
| --- | --- |
| `ply-mixed.png` | `5b145ebbaa87a4a2e852a9f79373fbd9c4f18cfa1aa4c839ecef8406ff37d717` |
| `ply-external.png` | `26f329e9a32e39f89926f7f5fdb8812d978852e0605f526866a42043608ac8e9` |
| `ply-curved-emitter.png` | `3b989151b0be2e5b44bfde079b642750e519b4cfbbaa00403067fa4b9fe33c35` |

Hosted CI and CodeQL evidence is attached to the PR against its final revision.
