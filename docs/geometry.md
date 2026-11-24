# Native geometry queries

The private geometry interface supports static indexed triangles, bilinear patches,
and affine instances. PBRT PLY quads retain their curved bilinear surface. The
[PLY validation record](pbrt-ply-validation.md) covers their numerical domain and
rendering integration.

## Query contract

Positions and transforms use float32, triangle indices use uint32, and transforms
are row-major three-by-four object-to-world affine matrices. Instances refer to
meshes by ordinal. Triangles are opaque and two-sided, with original vertex order
preserved under translation, rotation, nonuniform scale, shear, and reflection.
Bilinear patches are two-sided and store tensor corners `(p00,p10,p01,p11)`.
Rendering separately interpolates normals and applies orientation overrides;
alpha visibility, masks, motion, and refitting remain deferred.

The move-only scene handle owns native acceleration structures containing the
scene data, together with execution resources. Temporary upload and scratch
buffers are released after construction completes. Caller storage may be
destroyed immediately after a successful build. Queries are synchronous and must be externally
serialized. Moving transfers ownership; querying a moved-from handle fails.

A ray is `p(t) = origin + t * direction`. Its direction need not have unit length;
affine instancing does not change this parameterization. Origins and directions
must be finite, directions nonzero, and bounds satisfy `0 <= t_min < t_max` with
positive infinity allowed for `t_max`. Closest-hit queries return hit/miss,
mesh/instance/primitive ordinals, `t`, and barycentric weights in original vertex
order for triangles. Patch hits return a primitive-kind tag and `(u,v)` in the
closed unit square. Occlusion returns a Boolean after the first independently
validated accepted native candidate.

Triangle queries use Metal bounding-box acceleration structures and intersect
offered boxes against original per-primitive vertices. This avoids relying on
hardware triangle tests that omitted a true neighbor in a captured shared-edge ray.
Compensated forward
affine products and projected edge functions resolve near-edge false positives
and distances that collapse to the same native float. Ordinary arithmetic uses
guarded float pairs; exceptional intermediate range recomputes the full triangle
with integer binary24 pairs carrying independent, bounded exponents. These are
finite-precision predicates, not exact geometry. Comparisons of their represented
distances use exact signed dyadic sums. Closest-hit queries do not commit native
triangle distances. A conservative forward-transformed world-box exit may shorten
the traversal range through a query reset; the original ray bounds still govern
primitive classification. Unrepresentable bounds simply disable that pruning.
Normal-domain exponent operations and FMA residual checks preserve the guarded
pair arithmetic. World enclosures use binary32 only when each operation can
produce the same extended bound; other inputs retain the integer construction.
Forward-transform scalar products specialize only when both normalized operands
have binary32 exponent 127. Their rounded product and exact FMA residual form
the same pair as the general multiplication: the residual lies on the `2^-46`
lattice and is normal or zero. The guard retains the general fallback, incoming
invalid state and validation of both rescaled limbs. This changes execution cost,
not the finite-pair intersection policy.
When a reset offers the current-best triangle again, its immutable solve is
reused while alpha is called again with the same validated coordinates and
distance. Rejection retains the previous best; invalid alpha still aborts.

Eligible packet scenes also build native opaque-triangle structures. Their
offers are validated against original vertices and the original world ray before
use. A shadow hint must identify a definite opaque blocker. A closest hint seeds
the complete compensated distance and surface record before procedural traversal,
which can still replace it with a nearer alpha, analytic, patch or triangle hit.
An unavailable conservative cap leaves the original query interval intact; an
empty procedural offer stream retains the accepted seed. Native misses and
unsupported optional solves fall back to the existing traversal. Public geometry
and stateful alpha callbacks keep their original paths.

Eligibility is fixed by scene contents: packet transport, at least one opaque
triangle placement, and no null/invisible passthrough, mix/glTF material or
differing medium interface. Allocation and configured/device budget failures
are reported; resource availability never silently changes the numerical mode.
The extra structures preserve expanded instance ordinals and share primitive
records. This changes candidate coverage and requires renderer contract 20;
exact image controls are bounded evidence, not a universal identity guarantee.

Packet connection visibility uses Boolean occlusion when every accepted surface
blocks the segment. Scenes with any null-interface material or invisible-emitter
placement retain closest-hit boundary traversal. Both routes preserve endpoint
bounds and alpha validation; early termination need not visit the same candidates.
See the
[regression record](transport-regression-validation.md) for the numerical policy,
measured coverage and limits.

## Optional acceleration build policies

`AccelerationBuildPolicy` exposes `default_build`, `fast_blas`, `fast_tlas` and
`fast_all` through geometry/render build options and resident `FrameConfig`.
The benchmark selects them with `--as-build default|fast-blas|fast-tlas|fast-all`.
The default preserves existing descriptors. Fast policies are experimental and
require the native intersection profile, the macOS 26 SDK and runtime, Apple GPU
family 9 support and compute ray tracing. An unsupported explicit request fails
with a diagnostic; it never silently selects the default. Compensated/offline
builds retain their existing default behavior. Older SDKs compile the default
path without referencing the new preference symbol.

The retained six-scene screen advanced no fast policy to full qualification or
default selection. `default_build` remains the default. The
[build-policy screen](core-performance.md#native-build-policy-screen) records
its settings, image controls and allocation/build tradeoffs; these small-structure
results do not establish the policies' value for large geometry.

Selected BLAS and/or TLAS descriptors receive Metal's
[`preferFastIntersection`](https://developer.apple.com/documentation/metal/mtlaccelerationstructureusage/preferfastintersection)
usage before sizing, and the same descriptors are used for construction. This
also covers a resident empty-scene TLAS. Scratch sizing and existing allocation
budgets use the selected descriptor. Apple's preference may increase build time;
no renderer speedup or equal native edge behavior is established by enabling it.

Build results and benchmark JSON record requested/effective policy and capability
support (`null` before evaluation), actual applied BLAS/TLAS counts, and successful
retained AS sizes from [`MTLAccelerationStructure.size`](https://developer.apple.com/documentation/metal/mtlaccelerationstructure/size).
Host build wall time sums
intervals from sizing through allocation and GPU completion, including waits;
it excludes geometry preparation and pipeline construction. Resource counts and
sizes publish only on successful creation. A successful frame session also
records `MTLDevice.currentAllocatedSize` at creation: this is device-wide Metal
allocation accounting, including other owners, and is neither process RSS nor
session-exclusive memory. Transient scratch, driver overhead and unrelated
allocations must not be inferred from the retained-AS sum.

Portable checks cover defaults, enum mapping and CLI metadata. The dedicated
Metal check compares all four policies against an independent closest-hit and
occlusion oracle on ordinary mixed triangle/patch geometry with affine instances,
checks resident-session propagation and applied-level telemetry, and verifies
explicit rejection when the requested capability/profile is unsupported. These
bounded correctness checks do not replace scene-specific build/memory and
complete-pipeline measurements.

On 2026-11-23, the dedicated Debug check passed with Metal API validation in
0.52 seconds on M4 Max, macOS 26.6 and Xcode 26.6, for the uncommitted policy
implementation based on `4b1cc65`. All four policies applied their requested
levels and completed resident frames. This small fixture retained 1,216 BLAS
bytes and 1,216 TLAS bytes under each policy. The duration is a correctness-test
duration, not a build or rendering performance result. Real older-SDK/runtime
execution remains unmeasured; the legacy guard branch passes strict warning
compilation against installed headers with the SDK-version macro reduced.
The expanded test also passes under API validation in 9.21 seconds: scalar and
packet empty scenes under all four policies each retain one 448-byte TLAS and
zero BLAS allocations, apply the requested TLAS preference, and complete every
sample. These measurements precede stack integration.

## Explicit native triangle profile

`GeometryBuildOptions` and `RenderBuildOptions` accept
`IntersectionProfile::native`; omitted options retain `compensated`. The native
profile makes Metal triangle BLAS/TLAS intersection distances and barycentrics
authoritative for every triangle placement. It does not use the opaque-hint
eligibility exclusions for alpha, mix/glTF, null surfaces, invisible emitters or
medium interfaces. Triangle culling remains disabled at traversal; material
visibility applies through the existing callback using native barycentrics.
Source/primitive ordinals, affine instances and authored winding retain their
existing mapping. Bilinear patches and analytic primitives keep their procedural
predicates in the same primary TLAS, competing with accepted native triangles.
Accepted binary32 distances bound traversal; predicates retain the original ray
interval, including finite visibility endpoints.

This is a distinct numerical profile. Hardware near-edge false candidates,
missing neighbors, rounded distance ties and endpoint inclusion can differ from
the compensated profile. There is no compensated revalidation or recovery query
in native mode. Failure to build or specialize native resources fails the build;
it never selects the other profile. The existing offline progressive driver
continues to use compensated traversal. Native render sessions require a separate
version-1 shader handshake that executes function constant 21 and verifies that
the native specialization is active; the offline checkpoint contract is unchanged.
Consumers must keep accumulated samples from different profiles separate.

`metal.native_intersection_profile` supplies independent ordinary triangle and
occlusion checks across affine transforms, winding, two-sided rays, zero-area
slots and finite segment bounds. It also checks alpha rejection/invalidity,
procedural patch/analytic composition, scalar and packet glTF coverage and mix
shape alpha, null/invisible passthrough and a scalar Beer–Lambert slab. The slab
uses 16,384 analog free-flight paths over four predefined seeds and compares the
mean to `exp(-0.5)` with a predefined Hoeffding bound (false rejection probability
`1e-9`) plus `4e-5` numerical allowance; individual paths must remain zero or one.
The existing unsupported packet-medium diagnostic remains covered. Captured edge
witnesses print native and independent-oracle results as characterization,
without requiring native results to equal compensated results.

On 2026-11-22, this test passed in 74.18 seconds on an M4 Max 40-core GPU,
macOS 26.6 and Xcode 26.6, for the uncommitted implementation based on `aaf3996`.
The slab mean was 0.605469 against 0.606531 expected, within the predefined
0.0256052 tolerance. The native profile selected primitive 556 where the Day-ray
oracle selects 601, and primitive 0 where the missing-neighbor oracle selects 1.
See the [native-profile validation record](transport-regression-validation.md#opt-in-native-intersection-profile)
for commands, repetitions and scope. This is separate from older inherited
measurements and does not establish default-profile regression coverage.

Input validity does not guarantee that the resulting bounds, inverse transforms,
or hit parameters are representable in Metal's float32 arithmetic. For example,
a finite nonzero direction can produce a hit parameter above float32's maximum.
Such extreme ranges are outside the validated numerical domain; accepting finite
inputs is not a promise of exact or robust traversal for every such scene.

Validation rejects nonfinite geometry, invalid indices, singular transforms,
invalid rays, and sizes that exceed representation or device limits. Zero-area
triangles retain their primitive slots as explicitly empty box records. Nonzero triangles
have no arbitrary area cutoff. Empty scenes return
misses; empty ray batches return empty results. An operation that fails exposes
a diagnostic rather than partially valid query results.

Degeneracy validation uses double-precision determinant estimates with an
error-free expansion fallback for uncertain zero tests on binary32 inputs. This
avoids rejecting a nonzero determinant merely because its terms nearly cancel;
it does not make native traversal exact or well-conditioned.

Exact native interval endpoints and isolated outer triangle edges/vertices have
no stronger inclusion promise than Metal supplies. Tests characterize those
boundaries separately. Shared interior edges and vertices must not create holes:
a ray may select either incident triangle, but it must hit the surface. Equal
computed compensated-depth ties select stable native instance/geometry/primitive
IDs. The finite-pair predicate uses closed computed edges and bounds. Its own
roundoff can also move an exact geometric endpoint across a computed bound; it
cannot recover a candidate omitted by native traversal. Detected
unsupported arithmetic, including subnormal interval endpoints, fails visibly;
this is not a universal exact-intersection guarantee.

## Native execution

The builder selects the system-default device once. It distinguishes disabled
Metal, no device, unsupported compute ray tracing, and failures. There is no
automatic device substitution or CPU traversal fallback. An empty scene still
requires an available ray-tracing backend.

Each nonempty primitive kind in a mesh has its own native acceleration structure;
The adapter retains separate box structures for triangles, patches and analytic
shapes. A mixed mesh placement therefore expands to multiple native instances.
The scene instance structure and readback mapping preserve source ordinals. Construction and query stages check command
completion before consuming results. ARC retains the geometry, structures, and
execution resources, and the compute encoder declares referenced child structures
resident. CPU readback occurs only after successful completion. Diagnostics name
the failed stage and include available Metal errors.

Standard native limits are enforced before construction: at most `2^28`
primitives per child structure and `2^24` expanded native instances. Empty meshes contribute
no native structure; source mesh and instance ordinals remain stable.

Explicit host/MSL wire records have compile-time layout checks. Rounded dispatches
bound-check the logical ray count and verify untouched output guards. Geometry
MSL is compiled with fast math disabled into its own AIR object, then linked with
the compute shader into the existing executable-relative `rypath.metallib`.
The deployment minimum remains macOS 14 with MSL 3.1.

## Independent numerical checks

The test-only oracle uses double-precision forward transforms of the original
float vertices, plane intersections, and projected edge functions. It does not
reuse the Metal transform packer or acceleration traversal. Analytic fixtures
check the oracle itself. The reference is neither an optimized CPU renderer nor
an exact-arithmetic guarantee for arbitrary ill-conditioned geometry.

For bounded, well-conditioned fixtures, tests declare these budgets before GPU
comparison, with `eps = FLT_EPSILON`:

- Distance: `abs(t_gpu - t_ref) <= 32 * eps * max(1, abs(t_ref))`.
- Each barycentric weight: absolute error at most `64 * eps`.
- Each reconstructed position coordinate: error at most `64 * eps * scale`,
  where `scale = max(1, ||origin||inf + abs(t_ref) * ||direction||inf,
  max_i ||world_vertex_i||inf)`.

Hit/miss, occlusion, and uniquely determined IDs must agree exactly. Tie cases
validate a returned candidate's own barycentrics and reconstructed position.
Numerical budgets are not generalized to extreme coordinate scales or arbitrary
near-parallel configurations. Threshold changes require a numerical explanation,
not repeated runs until the GPU result passes.

The suite includes analytic front/back hits, asymmetric barycentrics, nearer and
farther surfaces, bounds, parallel and grazing rays, shared edges and vertices,
and transformed/reused meshes. Deterministic batches of 1 and 257 rays reuse the
same owner after construction inputs leave scope. Invalid inputs, empty workloads,
repeated queries, guard writes, and corrupted comparison results are checked.

## Commands and availability

Run all tests with the standard configure/build/test presets. Select geometry
and native validation after building Debug:

```sh
ctest --preset debug --no-tests=error --verbose -R 'geometry'
MTL_DEBUG_LAYER=1 ctest --preset debug --no-tests=error --verbose -R '^metal\.'
```

Default native tests require a device with compute ray-tracing support. Hosted
macOS CI explicitly enables `RYPATH_ALLOW_NO_METAL_DEVICE` and
`RYPATH_ALLOW_NO_METAL_RAY_TRACING`. Each allows only its corresponding observed
availability result to skip, with a visible reason. Ordinary compute tests still
run on a device without ray tracing. Build, validation, allocation, execution,
and mismatch failures never qualify for these skips. Portable builds run the
reference and explicit stub contracts.

Acceptance requires strict final-revision Debug, Release, and ASan/UBSan passes
on the M4 Max plus Metal API validation. See [status](status.md) for measured
evidence. Hosted availability is recorded separately. Native ray execution on a
named device does not establish a speedup or prove how every supported GPU
implements traversal internally.

## Primary references

- [Apple: Your guide to Metal ray tracing](https://developer.apple.com/videos/play/wwdc2023/10128/)
- [Apple: Standard and extended acceleration structure limits](https://developer.apple.com/documentation/metal/mtlaccelerationstructureusage/extendedlimits)
- [Metal Shading Language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)
- [PBRT: Applying Transformations](https://pbr-book.org/4ed/Geometry_and_Transformations/Applying_Transformations)
- [Woop, Benthin, and Wald: Watertight Ray/Triangle Intersection](https://jcgt.org/published/0002/01/05/paper.pdf)
- [Shewchuk: Adaptive Precision Floating-Point Arithmetic and Fast Robust Predicates](https://people.eecs.berkeley.edu/~jrs/papers/robustr.pdf)

These inform the contracts and tests. Metal owns acceleration-structure traversal
and candidate generation. Manual queries intersect triangles from embedded
vertices and solve bilinear/analytic box candidates from embedded records, retaining
the matching primitive and coordinates with the selected distance. The portable
references are independent test oracles.

## Opt-in opaque Boolean shadows

`ShadowTraversal::opaque_auto` specializes Boolean shadow visibility within the
native intersection profile. `query` remains the default. The host checks the
same prepared geometry used to build the primary acceleration structure, after
world-space emitter baking. Eligibility requires triangle-only geometry with no
retained zero-area/miss slots, nonopaque instances, shape alpha, mix/glTF
materials, null/invisible surfaces, media records, medium interfaces or volume
transport. These exclusions are conservative, including unused materials and
meshes. Ineligible requests retain query traversal and report the reason.

Opaque mode also requires bool function constant 22 in each actual unspecialized
render entry and selected scalar/packet wavefront shade entry. Missing capability
fails session construction with an explicit rebuild diagnostic, without publishing
effective opaque execution. A native-profile handshake alone is insufficient:
Metal can silently ignore specialization values for unknown constant indices.
Query mode keeps accepting native-compatible libraries without this capability.

The specialized helper uses `intersector<triangle_data, instancing>`, forced
opaque geometry, disabled culling and acceptance of any intersection. It returns
only Boolean occlusion plus validity. Existing ray-domain validation, visibility
masks, offsets and finite segment bounds remain in place. Invalid rays/results
fail visibly. Closest-hit traversal, surface IDs/ties, compensated traversal and
its separate optional opaque hints retain their existing policies. This is an
execution specialization of the native profile, with no sampling or estimator
change. It remains opt-in.

The declared Cornell target improved 4.09% in median paired complete-pipeline
p95, below the 10% adoption requirement. Query remains the default. The
[opaque-shadow qualification](core-performance.md#opaque-shadow-qualification)
retains every scene/seed result, including the non-target tail regressions.

Apple documents opaque triangle traversal and early any-intersection termination
in the [Metal Shading Language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)
and recommends opaque intersectors in its
[shader performance guidance](https://developer.apple.com/videos/play/tech-talks/111373/).
Those platform recommendations are not a measured rypath speedup.

`render.shadow_traversal` checks host eligibility and fallback reasons portably.
`metal.opaque_shadows` compares the production helper with the native query and
an independent double-precision brute-force oracle on ordinary affine,
shared-edge/vertex, grazing and segment-endpoint rays. It checks invalid inputs
and compares raw scalar/packet frames across megakernel and wavefront execution.
These fixtures are bounded correctness checks; native near-edge limitations
remain those of the selected intersection profile.

Before the subsequent opaque-capability guard, the working tree based on
`4b1cc65` passed Debug `metal.native_intersection_profile` in 72.47 s and `metal.opaque_shadows` in 16.87 s with Metal API
validation on M4 Max, macOS 26.6, Xcode 26.6 and SDK 26.5. These are correctness
test durations, not performance measurements. Raw XYZ matches the query control
exactly for scalar/packet and megakernel/wavefront frames at sample indices 0, 1
and 2^32+7. Portable host selection and benchmark CLI checks also pass.

The initial probe identified a native-profile grazing shared-edge limitation:
for the identity square at z=0, origin `(-.75,-.25,.001)` and direction
`(.5,0,-.001)` reach its shared diagonal, but both the existing native query and
opaque intersector report a miss while the double oracle reports a hit. The
fixture retains this ray as characterization with required query/intersector
agreement. A separate grazing interior ray requires independent-oracle agreement.
The final passing check does not imply that native traversal matches the double
oracle at all numerical boundaries. These initial checks preceded the later
integrated Release/sanitizer validation and measured policy comparison. See the
[current qualification record](core-performance.md) for those results;
`query` remains the default.

The capability regression fixture is compiled from test-only Metal source. Its
native handshake succeeds, while its legacy render/shade entries omit constant
22; a mixed new-main/old-shade case also requires rejection. The normal current
library still runs the raw controls. For local qualification,
`rypath_opaque_shadow_tests CURRENT.metallib CAPABILITY_FIXTURE.metallib LEGACY_NATIVE.metallib`
also checks that a frozen native-capable library remains accepted for query mode
and fails construction for opaque mode. External binary fixtures are not tracked.
After adding the capability guard, Debug `metal.opaque_shadows` passes in
17.50 s under Metal API validation on the same M4 Max environment. The current
scalar/packet render and wavefront shade entries expose the required capability;
the native-capable/mixed fixture rejects missing shade capability. A separate
run with the frozen native-capable, stage-diagnostics-only library exits 0:
query session construction succeeds and opaque construction fails with the
explicit missing-constant diagnostic, without publishing effective opaque mode.
The normal raw-image controls pass in that run as well. Full pre-commit hooks and
the portable CLI/selection checks pass. These results validate the capability
guard; they do not extend the earlier native-profile numerical guarantees or
establish a performance benefit.
