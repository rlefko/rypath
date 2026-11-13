# Architecture and product goals

## Implemented execution foundation

The current program is a C++20 command-line executable. CMake, Ninja, and CTest
provide the build and test contract. It accepts `--help`, `--version`,
`--metal-info`, `--metal-smoke-test`, `--render-demo`, `--check-pbrt SCENE`, and
`--render-pbrt SCENE --output IMAGE.png`. No arguments prints help, and invalid
arguments fail. Render the original demo or the supported PBRT spectral subset;
portable preflight remains independent of rendering.

A private C++ interface separates the portable CLI from a narrow ARC
Objective-C++ adapter on macOS. The adapter owns Metal resources through command
completion and loads a precompiled MSL library beside the executable. Other
platforms, and explicitly disabled Metal builds, use a stub that reports the
backend as unavailable. No third-party header distribution is required.

The integer dispatch check validates execution, buffer layout, bounds, and
readback. It does not change a physical model or Monte Carlo estimator. See
[Metal execution](metal-execution.md) for its contract and evidence requirements.

The geometry backend accepts private C++ indexed meshes, affine instances, and
ray batches. An immutable move-only owner retains one native triangle acceleration
structure per mesh and an instance acceleration structure, reusing geometry across
instances and synchronous queries. It owns its uploaded data independently of
the caller. Closest-hit results retain source IDs, world ray parameters, and
barycentrics; opaque occlusion uses a separate any-hit query. The test-only double
reference independently transforms vertices and scans all triangles. See
[native geometry](geometry.md) for numerical limits, ownership, and validation.

## PBRT preflight boundary

The portable reader owns source records, decoded tokens, authored numeric lexemes,
typed directives, dependencies, and diagnostics. A separate interpreter tracks
PBRT scene state and builds checked float/index geometry only within the declared
[compatibility profile](pbrt-profile.md). It preserves camera and material
descriptors without translating them to the demo renderer. Incomplete analysis
returns diagnostics and a partial inventory, without a complete scene handle.
No spectral reconstruction, light normalization, or renderer defaults are applied.

A separate portable translator consumes a complete checked scene and shared
effective-parameter selections. It owns narrowed camera, material curves,
environment/area-emission samples, and per-instance bindings, with source-located diagnostics
for the narrower [rendering profile](pbrt-rendering.md). It reuses checked geometry
instead of reinterpreting transforms. Only successful translation reaches Metal.
An owned filter record selects the original half-pixel box path or normalized
triangle sampling. Triangle camera arithmetic has dimension-aware support checks
in translation, progressive setup, and each native request. Samples beyond film
bounds remain attached to their source pixel with unit weight.
The render session owns uploaded variable-length spectral data. Film records
its sensor model and exposure independently of raw XYZ; the demo retains its
original relative response while PBRT uses calibrated finite-depth transport.

The upstream adaptations include the pinned Apache-2.0 tokenizer and sRGB
spectral reconstruction with its canonical coefficient/D65 data. A portable
reconstruction module embeds the table and supplies constant or sigmoid material
payloads; only compact per-material coefficients reach Metal. Numeric material
knots and dense emission samples retain their separate representations.
File expansion, typed parsing, scene interpretation, and reporting are implemented in rypath;
there is no linked PBRT renderer or runtime dependency. The
[dependency decision](pbrt-decision.md) records the integration boundary.

External PLY decoding now feeds the same checked geometry and shape bindings.
Mixed meshes retain triangles and true bilinear patches. Metal requires homogeneous
primitive structures, so one source mixed mesh uses separate triangle and procedural
patch structures, and one source placement may expand to two native instances.
The adapter maps both back to the source placement. Intersection tables belong to
their respective compute pipelines; accepted patch coordinates travel in an owned
per-ray payload. The [PLY record](pbrt-ply-validation.md) covers resource limits,
intersection conditioning, interpolation and area-light densities.

## Rendering and planned boundaries

The first renderer exercises these boundaries concretely: portable spectral
sampling and compensated film accumulation, an owned native render session and
complete-path MSL kernel, and a separate ImageIO PNG adapter. A narrow native
accessor reuses the existing geometry owner's device, queue, TLAS, and BLAS
residency. Host code receives complete samples rather than individual bounce
queries. Predetermined batches commit only after checked GPU readback and full
sample accounting. The [rendering record](rendering.md) specifies the Lambertian
environment estimator, MIS/roulette, wavelength and sensor conventions, and
bounded ray-origin behavior.

PBRT Sobol sampling uses a shared integer CPU/MSL implementation and pinned
direction/interval matrices in an owned read-only Metal buffer. Sampler settings
travel through the progressive driver and direct request boundary. Checked
absolute sample indices, fixed dimension assignments, and finite depth bounds
prevent table reuse or overflow. The demo and independent PBRT sampling retain
their original Philox sequence. See [Sobol validation](pbrt-sobol-validation.md).

Scoped PBRT area declarations bind independently of materials to directly placed
triangle meshes. The translator owns deduplicated dense emission tables and
source-instance bindings. The native session maps these to packed triangle
references and constant-time emitting-hit lookup. A separate uniform-area light
sample complements BSDF-hit emission through MIS; finite endpoint-to-endpoint
visibility reuses native any-hit traversal. The [area-light record](pbrt-area-validation.md)
states the estimator, numerical domain, and independent acceptance criteria.
Transformed emitters use checked world-space vertices for both native traversal
and shading, avoiding observed conditioning failures in highly anisotropic
object-space parameterizations. This preparation preserves authored records,
instance/primitive ordering, and effective orientation.

Inline meshes also own optional position-indexed normals and UVs. The native
renderer prepares a deduplicated normal table from each original source
mesh/linear-transform/orientation combination, before emitter baking. Common
scaling preserves relative vertex lengths until shader interpolation. Separate
geometric and shading normals govern ray offsets/emission and diffuse scattering,
respectively. UVs remain in checked/render data for future consumers; no textures
or tangent frames are implied. The [attribute record](pbrt-attribute-validation.md)
specifies numeric limits, affine instance semantics, and independent checks.

The offline renderer will separate scene ingestion, immutable render data,
transport, sampling, scattering, media, acceleration, accumulation, and image
presentation. Concrete APIs will be introduced with the first validated use case,
without a general plugin system or cross-platform GPU abstraction.

- **Execution:** C++20 host code, Metal Shading Language kernels, and Metal native
  triangle and instance acceleration structures. GPU resource ownership, buffer
  layouts, lifetimes, synchronization, and failure handling will be explicit.
  Small independent CPU reference paths will support differential validation.
- **Transport:** sampled-wavelength Monte Carlo paths with documented PDFs,
  wavelength/sensor weighting, light sampling, MIS, and compensated termination.
  Strict rendering will report conflicting or incomplete settings rather than
  silently clamp contributions, truncate paths, or discard unfinished samples.
- **Accumulation:** canonical raw radiance sums and counts remain separate from
  exposure, tone mapping, denoising, and preview approximations. Reproducible sample
  identities must survive scheduling changes, cancellation, and checkpoint/resume.
- **Scenes:** PBRT v4 is the scene-description standard. All loaders will target a
  versioned internal representation. Accepted syntax, faithful semantics,
  approximations, and unsupported features will be tracked separately. Missing
  materials, parameters, or geometry must produce actionable diagnostics.
- **Presentation:** PNG output is available; linear HDR output precedes a native application.
  Display-only edits will not invalidate raw samples; changes to the integrand
  will. Preview history and raw accumulation are distinct.

Metal API availability alone will not establish hardware ray-tracing support or
performance on a selected device. Custom acceleration structures are deferred
experiments requiring correctness and measured benefit. Geometry tests exercise
native ray queries; the CLI compute check remains separate. Neither introduces
a numerical transport estimator; the separate render mode does.

## Offline MVP acceptance

The offline MVP must combine progressive Metal rendering, a named and pinned
supported PBRT profile, indexed geometry and instancing, textures and lighting,
spectral surface scattering, homogeneous media, linear OpenEXR output,
checkpoint/resume, and cancellation. It also includes documented polygonal
OBJ/MTL, PLY, glTF/GLB, and STL import profiles.

Acceptance requires independent numerical tests, unmodified supported upstream
scenes with recorded overrides, and reproducible M4 Max measurements. Parser
success does not establish rendering compatibility, and selected corpus coverage
does not establish full PBRT support. Full OBJ includes non-polygon constructs;
those remain explicit roadmap work until rendered faithfully.

## Reference starting points

These define future investigations, not algorithms already implemented here:

- [Apple Metal C++ interface](https://developer.apple.com/metal/cpp/)
- [Apple Metal ray tracing](https://developer.apple.com/videos/play/wwdc2023/10128/)
- [PBRT v4 input format](https://www.pbrt.org/fileformat-v4)
- [Physically Based Rendering, fourth edition](https://pbr-book.org/4ed/contents)

Implementation decisions will cite primary research or official platform sources,
identify project adaptations, and distinguish published results from measurements.
