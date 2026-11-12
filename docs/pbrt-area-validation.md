# PBRT triangle area-light validation

This slice adds diffuse emission to directly placed triangle meshes in the
[PBRT rendering profile](pbrt-rendering.md). It changes the light model and adds
an area-light proposal to the estimator. It does not change material spectra,
the finite-depth model, wavelength sampling, film calibration, or the demo.
Selected numerical comparisons are evidence within a bounded domain, not a
proof of general PBRT equivalence or a performance claim.

## Model and estimator

An area declaration binds independently of material reflectance. Its emission
normal uses transformed geometric winding corrected by ReverseOrientation XOR
transform handedness, before the diffuse normal is face-forwarded. One-sided
emission uses the outward hemisphere; two-sided emission has the same radiance
on either side. Surface emission is evaluated before finite maxdepth and black
reflectance termination. Camera hits have unit weight. Scattered hits use the
weight complementary to the area proposal. Emitting surfaces may still scatter.

The pinned [PBRT diffuse emitter](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp)
densifies emission to integer nanometers. Numeric emission is divided by its
CIE-Y inner product; RGB emission and omitted-L D65 use the base D65 denominator.
Scale then applies. Positive power, textured emission, and emitting object
members are deferred. A zero numeric photometric denominator is invalid, while
RGB black and a valid spectrum with scale zero produce exactly zero radiance.

At each eligible diffuse vertex, the existing environment proposal remains
separate from one area proposal. With N emitting triangles, M=bit_ceil(N), each
triangle has probability q=1/M. Slots N through M-1 are explicit zero events.
There is no retry or energy/visibility-dependent renormalization. The masked
integer selection uses an unused Philox component. Triangle coordinates use
two other unused components: D=2^24, U=2*(word_u>>9)+1 and
V=4*(word_v>>10)+2. If U+V>D, reflect both about D. The three weights
(D-U-V,U,V)/D are strictly positive and exactly representable in binary32.
Vertex reconstruction can still round onto an edge. The PDF describes ideal
continuous uniform-area sampling, subject to ordinary finite-RNG discretization.

For distance r, world area A, and light cosine cL, pL=q*r²/(A*abs(cL));
the diffuse proposal is pB=cSurface/pi. Direct sampling uses
(pB/pL)*pL²/(pL²+pB²), while a subsequent emitting hit uses
pB²/(pL²+pB²). Scaled ratios avoid squaring overflowing densities. Roulette
compensation is unchanged, and its survival probability is consistently omitted
from both complementary MIS weights. Environment misses compete only with the
environment proposal. The separate endpoint domains avoid an extra selection
factor on sky illumination. See [PBRT's MIS path tracer](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer).

The uniform triangle proposal differs from PBRT's BVH light selection and
spherical-triangle sampling. An accepted `lightsampler=bvh` specifies the same
transport target here, not the same sampling implementation. Fixed sample indices
and the existing environment-only random dimensions remain unchanged.

## Finite visibility and execution limits

Direct and reverse area PDFs use the same spawned receiver origin and unoffset
emitter point. Visibility offsets the emitter toward that origin, with propagated
interpolation/transform/connection error, a world-extent traversal allowance,
and outward rounding. Sampled endpoints do not inherit native-hit barycentric
or object-extent allowances. An unnormalized
ray spans the endpoints with tMin=0 and tMax=nextafter(1,0). Native any-hit
traversal retains intervening blockers; no primitive is excluded and no fixed
fraction of the segment is discarded. [PBRT's rounding analysis](https://pbr-book.org/4ed/Shapes/Managing_Rounding_Error)
motivates the construction; Metal's undocumented traversal error prevents a
universal scene-scale guarantee.

Emitter world-space geometry must have representable positive area and a usable
normal. Conservative absolute-sum checks reject overflowing transform
intermediates even when the exact result would cancel into range. Nonidentity
emitter transforms are applied through one checked binary64-to-binary32 host
conversion before device acquisition. Acceleration and shading use the same
prepared world vertices with identity placement, retaining original instance/
primitive ordering and effective winding flags. Shared meshes are cloned only
for affected emitter placements; caller data and authored records remain intact.

This preparation addresses measured M4 Max false intersections when a small
world-space emitter was represented with object-space Y multiplied by 1,000,000
and an inverse instance scale. Normalizing the connection ray did not repair
the false intersections. Enlarging the offset would skip real blockers, so the
regression retains its near-light blocker at a relative gap of 2^-16.

There is no arbitrary area threshold or scene-wide minimum gap. Endpoint
uncertainty neighborhoods remain a documented limitation. Nonfinite or collapsed
positive connections fail the pending batch rather than being classified as
occlusion or selectively discarded. GPU buffers and acceleration structures
remain owned through completion, layouts/counts are checked, and padded output
guards precede atomic film publication.

The PBRT profile excludes shared emitting object instances and bounds imported
data. The private direct `RenderScene` API still permits much larger shared
inputs: repeated transformed emitters can require substantial host validation
and cloning before device buffer limits apply. Index growth and allocation
failures are checked; no separate host-memory quota or arbitrary-scale guarantee
is claimed for that internal API.

## Fixed acceptance criteria

Portable cases cover binding scopes/reset, effective defaults and color spaces,
material independence, object restrictions, dependencies, bounded storage,
source locations, and invalid numerical conversion. GPU cases cover depth-zero
and final-hit emission, sidedness/reflection, multiple lights and null slots,
finite blockers, repeated queries, guards, and failed-batch preservation.
The pinned mixed fixture covers combined scattering under environment and area
illumination. Visibility tests cover unit and 65,536 scales, near-source and
near-light blockers, geometry beyond the endpoint, and equivalent anisotropic
parameterizations with unique and shared meshes.

The independent direct-light oracle integrates a real jittered camera footprint,
not a zero-width camera. The receiver is z=0 with x,y in [-1/8,1/8], viewed from
(0,0,-2). Its normal is -Z. The off-axis emitter at z=-1 has vertices
(1/4,-1/2,-1), (3/2,-1/2,-1), (1/4,1/2,-1), with +Z winding. Independent
spherical-polygon and Duffy quadrature give directional mean
0.0930025220483664 and variance 0.00170612894641743 for unit albedo/radiance.
Spectral first/second moments use independent double integration over the
nearest-nanometer lookup intervals. The GPU gate uses 32,768 fixed-seed paths,
six analytic standard errors, and a numerical floor of
`2e-5 * max(1, abs(reference))`.

The [fixture manifest](../tests/fixtures/pbrt-render/README.md) contains nine owned
GPL fixtures and one labeled CC0 Cornell adaptation. The original Cornell and
notice remain byte-for-byte unchanged and unsupported. The adaptation changes
only sampler/filter, comparison film/sample settings, and removes N/UV arrays;
it retains camera, geometry, materials, emission, orientation, and maxdepth 65.
It does not establish unmodified upstream-scene support.

All ten comparisons use 8×8 pixels, 4,096 spp, and sixteen predetermined seeds
per renderer: rypath 0–15 and pinned CPU PBRT 16–31. This is 41,943,040 samples
per renderer. The four new fixtures use full-frame and four quadrant RGB gates;
the six existing fixtures retain their original gates. Every channel/region must
pass five combined standard errors plus the numerical floor above. Linear PFM
preserves negative and above-one values, and both inputs use savefp16=false.
Thresholds precede measurement; failures are investigated without seed rerolls.

## Evidence

Hardware: Apple M4 Max, 16 CPU cores, 40 GPU cores, 128 GB unified memory;
macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang 21.0.0
(`clang-2100.1.1.101`), CMake 4.0.3, Ninja 1.13.2. The system-default device is
`applegpu_g16s` with compute ray-tracing support. Deployment remains macOS 14 /
MSL 3.1. The pinned CPU PBRT source and submodules are clean, and its binary
SHA-256 remains `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.

| Check | Result |
| --- | --- |
| M4 Max Debug, Release, ASan/UBSan | 50/50 CTests per preset, no skips; 3.88 / 3.27 / 7.78 seconds |
| Metal API and shader validation | 7/7 native CTests, no skips; 2.38 seconds |
| Metal-disabled ASan/UBSan | 45/45 portable/stub CTests |
| LLVM 18 clang-tidy / Apple static analyzer | All 22 C++ / five Objective-C++ sources passed |
| Policy, PFM, fixture provenance tools | 13 Python tests passed |
| Pinned CPU PBRT comparisons | All 102 region/channel gates passed; 20.45 seconds total wall time |

Tensor eight-point Gauss-Legendre quadrature gives spherical mean
0.0930025220483668 and independent Duffy mean 0.093002522399731, a difference
of 3.52e-10 against the fixed 1e-9 gate. Its variance is 0.00170612905853831;
an independent review reproduced the higher-order reference using 16 and 24
points. The 32,768-path XYZ results were:

| Channel | Independent mean | GPU mean | Six-SE plus numerical bound |
| --- | ---: | ---: | ---: |
| X | 4.09405977609 | 4.11694260662 | 0.24272268087 |
| Y | 4.14199145671 | 4.11462205410 | 0.23579504656 |
| Z | 1.89736494316 | 1.93019521374 | 0.14781032249 |

Across all ten fixtures, the maximum error/bound ratio was 0.6255, in the
pre-existing RGB multibounce bottom-right red gate. New full-frame means were:

| Scene | rypath linear RGB | PBRT linear RGB |
| --- | --- | --- |
| Visible area | (0.78807781, 1.8948472, 3.0629088) | (0.7866487, 1.8962773, 3.0638808) |
| Direct area | (0.082485081, 0.071951816, 0.031616773) | (0.082531621, 0.071972674, 0.031654986) |
| Mixed area/sky | (0.2505856, 0.17359716, 0.12971217) | (0.25068202, 0.17356509, 0.12993202) |
| Adapted Cornell | (0.19611546, 0.12621734, 0.034579836) | (0.19587329, 0.12725669, 0.034961655) |

Independent reviews checked scene-state semantics, spectra, MIS, resource
ownership, quadrature, exact fixture variants, and all 320 PFM files. They
prompted regressions for malformed spectral dependency inventory, overflowing
transform intermediates, near-emitter clipping, and native intersections under
anisotropic parameterization. The original Cornell and notice retain their
hashes, and the derivative is verified against an exact allowlist of byte edits.

Routine CTest stays offline; pinned CPU reference comparisons are opt-in and
separate from CI. Hosted availability, CodeQL results, measured CI duration, and
final revision identity are attached to the PR. Hosted skips never replace
strict M4 Max execution. CodeQL covers C++; Apple static analysis and native
tests cover Objective-C++ and shader execution separately. These timings are
validation-run observations, not renderer performance claims.
