# PBRT triangle-attribute validation

This slice retains inline triangle UVs and renders per-vertex shading normals in
the existing diffuse [rendering profile](pbrt-rendering.md). It changes the
declared surface model and its sampling frame, while preserving spectral
calibration, random dimension assignments, complete sample batches, and raw XYZ
accumulation. UVs are retained in owned checked/render geometry; the supported
constant materials and emission do not consume them. Texture evaluation,
tangents, normal maps, and ray differentials remain unsupported.

## Attribute and transform contract

The pinned [PBRT triangle implementation](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/shapes.h)
interpolates transformed normals before normalization. The loader retains
`normal N` (and the documented `normal3` alias) and `point2 uv`, including normal
lengths, zero normals, degenerate UVs, and UV values outside [0,1]. Arrays share
the position indices and must have one element per vertex. Omitted arrays remain
absent; PBRT's default UVs are per triangle corner, not per shared vertex. Legacy
`st` and float UV forms remain unsupported. Malformed arrays and unrepresentable
float components receive source-located errors.

Retained P, indices, N, and UV scalar values count against the structural and
translation budgets. A separate translation processing budget charges three
normal components per placed vertex, including repeated placements, to bound
portable normal validation work. Native normal storage is deduplicated by source
mesh, linear transform, and effective winding flag; translation does not enter
that key. Geometry remains instanced in Metal's native acceleration structures.

For linear transform A, the double-precision cofactor matrix is
`C = det(A) * inverse(transpose(A))`. Multiplying C by the existing
`reverse_orientation XOR mirrored` sign produces the authored normal transform
times a common positive determinant magnitude. A common power-of-two scale puts
the largest component of the complete table in [0.5,1). No vertex is normalized
individually. This avoids determinant division and retains relative vertex
lengths. A transformed nonzero component that becomes zero or subnormal at
packing is an error: even a tiny component can decide an emitter's side.
Other component rounding and cancellation remain finite-arithmetic limitations;
no exact-arithmetic or arbitrary-conditioning guarantee is made.

Normal preparation uses the original source instance before emitter vertices
are baked into world space. Native/source mappings survive emitter clones.
Host/device/index limits precede buffer allocation, and the render session owns
its normal buffer through GPU completion. Portable validation and Metal upload
use the same preparation helper.

PBRT applies instance orientation in two stages. For the currently supported
nonemitting, two-sided diffuse instances, using the composed inverse transpose
can differ only in normal sign: the outgoing-facing diffuse frame and
direction-facing geometric offset are invariant to that sign. This is not a
claim of support for instanced emission, normal derivatives, or future one-sided
materials.

## Transport and emission

The shader keeps geometric and shading normals separate. A nonzero barycentric
normal sum is normalized after max-component scaling; exact computed zero uses
the geometric fallback. Diffuse evaluation, cosine sampling, and competing MIS
densities use the outgoing-facing shading hemisphere. Exactly tangent outgoing
directions stop scattering after endpoint emission. There is no geometric
hemisphere clipping or extra shading-normal correction in this radiance path,
matching the pinned [BSDF](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/bsdf.h)
and path-integrator convention. Shading normals are a surface-model convention,
not a claim of an energy-conserving replacement for arbitrary geometry.

Each continuation or visibility ray is offset along the geometric plane normal
toward its actual direction. Samples crossing the plane remain valid. For
vertices with authored normals, area proposals and complementary emissive-hit
MIS use the same unoffset surface reference; geometric offsets affect traversal
only. The finite displacement still perturbs actual hit locations and visibility
within the renderer's existing numerical limits. Attribute-free paths preserve
their previous reference-point arithmetic and random streams.

Emission uses the geometric normal oriented toward the interpolated authored
normal; the area Jacobian always uses the absolute geometric cosine. Opposed
and mixed-sign fields are supported. Hit and area-sample paths share the base
geometric normal at exact zero/tangent orientation ties. Pinned PBRT differs at
such ties when raw and effective winding disagree. To avoid a positive-area
semantic substitution, a one-sided emitting triangle is explicitly unsupported
when all three packed corner normals are tangent/zero and its effective winding
is reversed. One-sided emitters also reject nonzero plane components lost or
sign-reversed by the ordinary binary32 cross product (checked against double
products of the rounded edges), components lost during normalization, and fields
whose three corner projections are all numerically
indeterminate. The latter uses `8 * FLT_EPSILON * sum(abs(products)) +
4 * FLT_MIN` around the packed-normal/geometric-plane dot product. An unflipped
exact coordinate-wise zero projection remains accepted. This conservative
numeric profile avoids choosing an arbitrary emitting side for an entire face;
it is not a universal bound on interpolation conditioning. Two-sided emission
is unaffected. Isolated tie lines remain a finite-arithmetic convention.

## Acceptance criteria fixed before measurement

Portable checks cover attribute ownership and indexing, scopes and precedence,
cardinality and narrowing diagnostics, processing/storage limits, shared
instances, unequal normal lengths, composed affine transforms, normal range,
and the narrow degenerate-emitter rejection.

Native checks include both sides of a tilted-normal constant-environment
surface, directional area illumination across its geometric plane, a nearby
blocker, unequal per-vertex lengths, transformed instances, emitter sidedness,
zero/cancelling normals, ownership, and progressive sample accounting.
Analytic radiance checks use fixed seeds and six standard errors plus
`2e-5 * max(1, abs(reference))`. A furnace alone cannot distinguish ignored
normals; directional area quadrature supplies that independent sensitivity.

Four additional reference fixtures cover varying normals, affine instances,
emitter orientation, and the attributed Cornell scene. All use the existing
8×8, 4,096-spp protocol, with sixteen seeds per renderer (rypath 0–15, CPU PBRT
16–31), full-frame/quadrant RGB regions, and five combined standard errors plus
`2e-5 * max(1, abs(reference))`. Existing fixtures and thresholds remain unchanged.
The [fixture manifest](../tests/fixtures/pbrt-render/README.md) records exact
Cornell overrides. Its original upstream bytes remain unchanged and unsupported
only because of Sobol sampling; the comparison variant is not an unmodified
upstream-scene rendering claim.

## Evidence

Hardware: Apple M4 Max, system-default `applegpu_g16s` with compute ray tracing;
macOS 26.6 (25G5065a), Xcode 26.6 (17F113), CMake 4.0.3, Ninja 1.13.2.
Deployment remains macOS 14 / MSL 3.1. The pinned CPU PBRT source is
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`; its Release/float binary SHA-256 is
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.

| Check | Result |
| --- | --- |
| M4 Max Debug, Release, ASan/UBSan | 55/55 CTests each, no skips; 8.97 / 5.08 / 17.37 seconds |
| Metal API and shader validation | 9/9 native CTests, no skips; 7.65 seconds |
| Metal-disabled ASan/UBSan | 48/48 CTests; 5.05 seconds |
| LLVM 18 clang-tidy / Apple static analyzer | All 27 C++ / five Objective-C++ sources passed after test lint fixes |
| Hooks and Python policy/reference tools | Repository hooks and 14 Python tests passed |
| Pinned CPU PBRT comparisons | 177/177 gates, including all 60 added gates; 480 PFM outputs |

The directional tests use 32,768 paths and seeds `0x7eab129ff367c4d2` and
`0x15adb51978ed3909`. Double camera/emitter quadrature refines from 16×16 raster
points with 192×192 emitter points to 24×24 with 384×384; mean and second-moment
differences must remain below `2e-6`. The minimum expected BSDF-hit arm is 0.005,
so passing requires meaningful complementary-hit coverage.

| Scalar radiance case | Reference | Observed | Absolute acceptance bound |
| --- | ---: | ---: | ---: |
| Tilted furnace, front | 0.625 | 0.6259840826 | 0.0052718091 |
| Tilted furnace, back | 0.625 | 0.6253166599 | 0.0052718091 |
| Cross-plane area illumination | 0.1052231331 | 0.1050333300 | 0.0029803234 |
| Affine varying normals | 0.0795562988 | 0.0793968892 | 0.0023590517 |

The nearby blocker was exactly black. Zero-normal fallback matched the geometric
control for 1,024 samples even with different UVs. Exact cancellation at known
barycentrics retained geometric emitter orientation, and rays on either side
switched sides under opposite mixed-sign fields. Exactly tangent outgoing rays
terminated normally. Batch partition and threadgroup widths 32/64 preserved the
same sample identities and radiance after source storage destruction.

The final Release reference comparison includes the attributed Cornell with its
original radius-one triangle filter. Its full-frame linear RGB means were
(0.19003643, 0.12431782, 0.034265345) for rypath and
(0.19160426, 0.12503266, 0.034551816) for PBRT. The maximum new error/bound ratio
was 0.425 in the transformed-instance full-frame green channel; the overall
maximum was 0.625 in the existing RGB multibounce bottom-right red channel.
These are validation results, not renderer performance claims.

Independent reviews checked normal transformation, authored/source ownership,
host/shader ABI, shading-hemisphere support, MIS references, visibility offsets,
input diagnostics, and resource bounds. Review added regression cases for lost
normal components, geometric-plane normalization, and cross-product cancellation
that could otherwise change a whole face's emitting side. Each now fails visibly
within the documented numeric profile.

The pinned CPU reference remains an ignored, opt-in validation tool; routine
CTests require no network or external scene downloads. Hosted CI and CodeQL
results are recorded on the PR against its final revision.
