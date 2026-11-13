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
closed unit square. Occlusion issues a separate native any-hit query and returns
a Boolean.

Input validity does not guarantee that the resulting bounds, inverse transforms,
or hit parameters are representable in Metal's float32 arithmetic. For example,
a finite nonzero direction can produce a hit parameter above float32's maximum.
Such extreme ranges are outside the validated numerical domain; accepting finite
inputs is not a promise of exact or robust traversal for every such scene.

Validation rejects nonfinite geometry, invalid indices, singular transforms,
zero-area triangles, invalid rays, and sizes that exceed representation or device
limits. Nonzero triangles have no arbitrary area cutoff. Empty scenes return
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
closest-depth ties may select either valid nearest primitive.

## Native execution

The builder selects the system-default device once. It distinguishes disabled
Metal, no device, unsupported compute ray tracing, and failures. There is no
automatic device substitution or CPU traversal fallback. An empty scene still
requires an available ray-tracing backend.

Each nonempty primitive kind in a mesh has its own native acceleration structure;
Metal requires homogeneous triangle or bounding-box descriptors within each
structure. A mixed mesh placement therefore expands to two native instances.
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

These inform the contracts and tests. Metal owns triangle intersection and
traversal. Bilinear boxes use a custom intersection function with embedded corner
data; an accepted-hit payload preserves the matching primitive and UV tuple
through traversal. The portable references are independent test oracles.
