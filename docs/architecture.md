# Architecture and product goals

## Implemented execution foundation

The current program is a C++20 command-line executable. CMake, Ninja, and CTest
provide the build and test contract. It accepts `--help`, `--version`,
`--metal-info`, `--metal-smoke-test`, `--render-demo`, `--check-pbrt SCENE`,
`--render-pbrt SCENE --output IMAGE.{png,exr}`, `--check-obj MODEL.obj`, and
`--convert-obj MODEL.obj --output DIRECTORY`, `--check-stl MODEL.stl`, and
`--convert-stl MODEL.stl --output DIRECTORY`. STL modes accept
`--stl-colors auto|viscam|magics`. No arguments prints help, and invalid
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
[compatibility profile](pbrt-profile.md). It preserves camera, material, and
typed texture descriptors. Supported image consumers retain shared decoded PNG/JPEG/EXR/HDR data, packed mip
pyramids, and native-resolution views. Incomplete analysis returns diagnostics
and a partial inventory, without a complete scene handle.
No spectral reconstruction, light normalization, or renderer defaults are applied.

Inline bilinear meshes reuse the PLY quad representation without its cyclic-index
conversion. Loop subdivision is portable checked preprocessing: an indexed
manifold topology applies pinned refinement and limit stencils in object space,
producing shared triangles and unnormalized limit normals. Independent scalar,
scratch and cumulative-work limits bound expansion before allocation. Both
declarations reuse existing graphics-state registration and render translation;
no new GPU primitive or sampling contract is introduced. See
[mesh validation](mesh-shape-validation.md).

A separate portable translator consumes a complete checked scene and shared
effective-parameter selections. It owns narrowed camera, material curves,
environment/area-emission samples, and per-instance bindings, with source-located diagnostics
for the narrower [rendering profile](pbrt-rendering.md). It reuses checked geometry
instead of reinterpreting transforms. Only successful translation reaches Metal.
An owned filter record selects box, triangle, or normalized tabulated Gaussian
sampling. Gaussian CDFs are compiled portably and owned by the native session. Triangle camera arithmetic has dimension-aware support checks
in translation, progressive setup, and each native request. Samples beyond film
bounds remain attached to their source pixel with unit weight.
An optional perspective lens retains radius, focal distance and the captured
camera-plane axes, including reflected rigid poses. Uniform disk samples add a
normalized aperture integral with unit camera weight. Primary rays and texture
differentials share the realized binary32 aperture displacement, so world-origin
rounding cannot give them different focus planes. Dimension-aware validation
bounds the full filter/aperture support, including world-space rounding, before
translation completes and at progressive/direct-request boundaries. Zero radius
retains the original pinhole arithmetic. See
[depth-of-field validation](depth-of-field-validation.md).
The render session owns uploaded variable-length spectral data. Film records
its sensor model and exposure independently of raw XYZ; the demo retains its
original relative response while PBRT uses calibrated finite-depth transport.

The upstream adaptations include the pinned Apache-2.0 tokenizer and sRGB
spectral reconstruction with its canonical coefficient/D65 data. A portable
reconstruction module embeds the table and supplies constant or sigmoid material
payloads for non-image RGB. Numeric material
knots and dense emission samples retain their separate representations.
Translation validates all named constant textures and compiles each spectrum
definition once to an existing diffuse material payload. Inline and named
material bindings share that payload by index across object instances.
The shader evaluates the resulting constant, sigmoid, or sampled spectrum
through the diffuse path described in the [rendering profile](pbrt-rendering.md#constant-textures).
Blackbody environment and area emission compile on the CPU into the same dense
radiance tables as numeric spectra. Stable Planck ratios retain double precision
through photometric normalization; no new shader payload or estimator is needed.
The [blackbody validation record](blackbody-light-validation.md) describes the
numerical domain and independent evidence.
Named emission uses a shared registry of the 88 pinned PBRT spectra and immutable
one-nanometer tables. Structural dependency classification and render translation
resolve the same exact names; the existing checked photometric calibration and
Metal payload handle their emission. The [named-spectrum record](named-spectrum-validation.md)
documents data provenance and validation.
File emission is decoded during preflight into shared immutable binary32 knots,
under the existing combined scene/asset budgets. Translation uses these checked
knots without reopening files, and shares the numeric-spectrum interpolation and
light calibration path. File curves retain zero outside their recorded interval.
The [file-spectrum record](file-spectrum-validation.md) describes parser boundaries,
numerical adaptations, and independent comparisons.
Diffuse materials and spectrum constants share that checked-file ownership and
can also use original built-in knots. A portable curve compiler applies the
diffuse model's clamp after interpolation, using exact-product expansions to
locate clamp crossings without cancellation. Bounded compiled knots reuse the
existing CPU/Metal sampled-material payload. The
[reflectance record](spectral-reflectance-validation.md) defines the binary32
wavelength contract and its independent validation.
Blackbody diffuse reflectance retains a checked temperature and evaluates the
continuous, Wien-normalized Planck spectrum at the path wavelength. Portable
double and Metal float evaluators use a stable log-domain ratio; a tagged scalar
in the existing material wire record carries the GPU exponent factor. This
shares material/constant-texture bindings without changing the diffuse estimator.
The [blackbody reflectance record](blackbody-reflectance-validation.md) specifies
numerical tolerances and the device's underflow boundary.
The translator binds each imagemap by index and prepares per-placement triangle
partials from the checked shape and placement transforms. The image render
session owns UVs, triangle partials, texture descriptors, shared mip data,
transfer tables, and the sRGB reconstruction table. At each textured hit the
shader computes the footprint, evaluates RGB, and reconstructs albedo at the
path wavelength. The [image texture record](image-texture-validation.md) describes
the lookup and its validation.
Each texture selects point, bilinear, trilinear, or EWA lookup independently of the shared
pyramid. Trilinear blends decoded linear RGB across adjacent levels before
spectral reconstruction; the [trilinear record](trilinear-texture-validation.md)
defines its pinned PBRT boundary behavior.
Point lookup preserves signed texel rounding before wrapping and uses separate
UV scale and offset operations at discontinuities. The
[point record](point-texture-validation.md) defines its numerical extension for
extreme coordinates and exact lookup checks.
EWA descriptors additionally carry a bounded anisotropy limit. Both signed
derivative vectors feed elliptical filtering of the shared mip pyramid; the
[EWA record](ewa-texture-validation.md) defines stable arithmetic, bounded scan
work, and edge handling.
Sixteen-bit PNG sources retain their original channel bytes and compile to linear
binary16 mip storage. A private format discriminator selects sRGB8, linear8, linear-half, or linear-float texels in the existing Metal descriptor. Shared buffer offsets count
32-bit words: one per encoded-byte texel or two per half RGB texel. Both backends
widen half subnormals without half-precision arithmetic; filtering remains float.
The [16-bit record](image16-texture-validation.md) defines quantization, memory
accounting, and reference coverage.
EXR sources retain channel type independently of their decoded float vectors:
HALF compiles to binary16 mips; FLOAT and Radiance RGBE compile to binary32
mips. Linear-float texels occupy four 32-bit words for RGB and optional scalar
alpha. Portable and Metal views share this format and its exact word accounting.
Floating samples ignore valid authored transfer encoding. Native views bypass
power-of-two mip resizing for normal maps and image lights; signed and HDR
values survive native storage; mip resizing and consumers apply the pinned PBRT
semantics, including the nonnegative floor after NPOT resampling.
The [HDR record](hdr-image-validation.md) defines decoder exclusions and
independent source/mip evidence.
Indexed PNGs decode to packed indices, validate every palette reference, and
expand to the existing RGB8 source representation. Preflight accounts for packed
and expanded storage before allocation. The existing image caches, mip compiler,
and Metal payload handle the resulting RGB8 image without an additional format.
The [indexed record](indexed-texture-validation.md) defines the bounds and evidence.
Packed grayscale PNGs use the existing decoder conversion into RGB8. The same
source ownership, transfer, mip, and Metal paths apply, with expanded bytes
charged to the existing resource budgets. The
[grayscale record](grayscale-texture-validation.md) defines the exact expansion.
File expansion, typed parsing, scene interpretation, and reporting are implemented in rypath;
PNG decoding uses the vendored portable LodePNG decoder. The
[dependency decision](pbrt-decision.md) records the integration boundary.

External PLY decoding now feeds the same checked geometry and shape bindings.
Mixed meshes retain triangles and true bilinear patches. Metal requires homogeneous
primitive structures, so one source mixed mesh uses separate triangle and procedural
patch structures, and one source placement may expand to two native instances.
The adapter maps both back to the source placement. Intersection tables belong to
their respective compute pipelines; accepted patch coordinates travel in an owned
per-ray payload. The [PLY record](pbrt-ply-validation.md) covers resource limits,
intersection conditioning, interpolation and area-light densities.

The core texture layer compiles typed acyclic graphs to a bounded CPU/Metal
instruction representation. Definition and mapping validation precede upload;
image views remain immutable and shared. Surface evaluation receives position,
normal, UV, and explicit camera/fallback differentials. Alpha uses a separate
zero-derivative definition-frame context before instance placement. Native
intersection queries accept/reject triangle candidates and each admissible
bilinear root without stepping over coincident primitives.

Normal and bump maps use the shape-definition shading frame followed by placement;
PLY displacement rebuilds geometry before native acceleration. Image area lights
use native bilinear radiance with the existing area proposal. Equal-area image
environments have a bounded area-averaged importance grid and a uniform proposal
component with matched NEE/escape PDFs. Scene function constants remove graph,
manual-query, surface-detail, and image-light code from unused pipeline variants.
See the [core texture contract and validation](core-texture-validation.md).

The EXR publisher projects complete compensated XYZ means to linear sRGB using
the film's sensor model and exposure. Raw sums, corrections and counts remain
unchanged. It validates finiteness and selected precision, writes a sibling
temporary file through positioned callbacks, checks codec finalization and
close, then atomically renames. Failure preserves the old destination and removes
the temporary file. PBRT output honors `savefp16`; demo EXR uses FLOAT.
The bounded static C11 OpenEXRCore subset owns codec work through scoped allocation
callbacks and adds no installed-SDK product dependency. Metadata identifies the
render settings and overrides, without defining checkpoint identity.

## Spectral delta illumination

Checked point descriptors retain intensity parameters, declaration transforms,
and endpoint-medium names. Translation shares spectral emission preparation with
other lights and owns a bounded list of world positions and dense intensities.
The Metal session packs these records into its existing auxiliary-data arena,
with explicit offset/count bounds and host/shader layout assertions.

Surface and medium vertices sum all point lights using inverse-square intensity
and unit delta-position MIS weights. Finite shadow connections reuse alpha,
null-boundary and homogeneous-transmittance traversal. The geometry-free sky
kernel uses analytic homogeneous connections. Scene specialization removes point
work when absent, preserving existing sampler streams and continuous-light PDFs.
Canonical identity covers positions and intensities; renderer/identity contracts
advance together. See the [point-light profile](pbrt-rendering.md#point-lights)
and [validation](point-light-validation.md).

Owned spot records add a checked inverse direction map and cosine thresholds.
A shared tagged positional-light buffer packs points, spots, goniometric and projection
sources and feeds the same surface and volume visibility loops, including
geometry-free media.
Spot attenuation follows the pinned cubic cosine model; the point branch retains
its existing arithmetic. All four kinds share a 64-source bound and unit delta-position
MIS weight. See the [spot profile](pbrt-rendering.md#spot-lights) and
[validation](spot-light-validation.md).

Goniometric records add a native scalar image binding and the actual inverse
of the declaration's linear transform, followed by PBRT's Y/Z swap. Translation
accepts rigid transforms and reflections within the documented Gram tolerance;
it preserves the inverse without direction normalization or common rescaling.
Square gray/RGB/RGBA maps reuse the bounded image codecs and native storage.
RGB becomes a quantized scalar average; lookup uses the pinned equal-area map,
nearest texels and clamp boundaries. All positional visibility loops, including
geometry-free homogeneous media, apply the angular factor to spectral intensity
without additional random dimensions. Identity covers the ordered records and
owned map contents. See the [goniometric profile](pbrt-rendering.md#goniometric-lights)
and [validation](goniometric-light-validation.md).

Projection records own an RGB image binding, scalar scale, actual inverse affine
map with local Y flip, inverse half-FOV tangent and aspect-dependent screen
half-extents. A wavelength-aware positional helper performs nearest perspective
lookup and checked RGBIlluminant reconstruction for all three transport paths.
Scale/shear remain in the actual inverse, including the local near-direction
cutoff. Shared positional-image specialization retains image-free point/spot
sky variants. See the [projection profile](pbrt-rendering.md#projection-lights)
and [validation](projection-light-validation.md).

Distant sources have a separate bounded direction/radiance buffer and a shared
scene radius. Translation computes PBRT-style bounds from prepared geometry,
retaining object aggregation before instance transforms. Certified affine
arithmetic prepares the unit source direction. Surface and medium vertices sum
all distant sources with unit delta weight and no inverse-square factor or
additional random dimensions. Finite `2 * radius` connections retain their
original endpoint with PBRT's per-segment cutoff. Geometry-free distant scenes
are diagnosed explicitly. See the [profile](pbrt-rendering.md#distant-lights)
and [validation](distant-light-validation.md).

## Homogeneous volume state and transport

The checked PBRT scene owns homogeneous medium descriptors, including inherited
parameters, checked spectrum files and source locations. Graphics state carries
inside/outside names through attribute and object scopes; each shape captures its
binding. The camera captures its outside medium at declaration time. Translation
validates unused definitions, resolves global names and prepares an owned medium
table, a camera ID and per-instance inside/outside IDs. Empty names map to an
explicit vacuum sentinel. This preserves bindings across parser-storage release
and instancing.

Explicit `volpath` enables absorption, scattering and volume emission. The
existing `path` mode remains vacuum surface transport. Each homogeneous medium
owns dense binary32 absorption, scattering and emission spectra at integer
nanometers; CPU/MSL use the pinned nearest-wavelength lookup. Measured presets
retain their pinned source data and separate license. Photometric preparation
and medium-local sampling are specified in the
[render profile](pbrt-rendering.md#homogeneous-media).

Null boundary materials retain geometry for medium transitions while contributing
no BSDF or surface-depth event. Equal-side interfaces retain the incoming medium;
distinct sides select by geometric normal. This is explicit boundary state,
without an inferred nested IOR stack. Shadow transport crosses null boundaries
and evaluates medium transmittance; ordinary glass remains an occluder for direct
light shadow rays. Opaque boundary alpha and finite boundary-chain work are
validated. Resource, numerical and work-limit failures reject the pending batch.
Null departures use a ray-anchored normal offset with an outgoing-side error
certificate. Finite visibility retains a fixed endpoint across crossings;
see [bounded-fog validation](bounded-fog-validation.md).
Raw compensated XYZ accumulation remains independent of presentation.

## Spectral conductor scattering

Material scattering is separate from its diffuse spectral representation. Conductor
materials own two unbounded, nonnegative piecewise-linear optical curves and two
scalar roughness bindings. The existing knot and texture resources carry their
payloads; no optical input uses the albedo compiler. Definition and placement
transforms preserve shading tangents for anisotropic reflection, including surfaces
without texture graphs. Normal/bump mapping retains the modified tangent frame.

A checked single-scattering GGX evaluator and visible-normal sampler share a local
orthonormal frame. Smooth hits carry a delta event; rough hits use the same BRDF
and PDF for continuation, environment lighting and finite-emitter MIS. Rejected
visible-normal samples and zero Fresnel terminate continuation normally, retaining
earlier contributions. Invalid arithmetic fails the staged batch. Scene function
constants exclude conductor work from diffuse-only pipelines. The
[conductor validation record](conductor-validation.md) describes the model,
numerical adaptations and independent evidence.

## Spectral dielectric scattering

Dielectrics reuse the material wire layout, optical knot storage, roughness
textures and authored surface frames. Their signed local interface coordinates
select entering/exiting refraction, with Fresnel branch selection and radiance
transmission weights. Both hemispheres participate in continuous light/BSDF MIS;
smooth and index-matched events retain delta bookkeeping. Existing geometric
offsets and whole-batch failure handling apply to transmitted paths.

A shared CPU/MSL sampling contract appends a disjoint dielectric lobe stream,
preserving the original eight transport dimensions per bounce. Scene-dependent
Sobol depth validation runs in translation, the driver and direct Metal requests.
Sampler provenance identifies the extension. See the [render profile](pbrt-rendering.md#spectral-ggx-dielectrics)
and [validation record](dielectric-validation.md).

## Extended surface scattering

Tagged materials reference indexed auxiliary payloads; immutable measured/Fourier
tables are deduplicated in a checked shared Metal buffer. Translation resolves
named material graphs and compiles spectrum textures by consumer role. A stable
material ID resolves a nested mix at a hit before the child's bump/normal frame.

The scattering contract separates evaluated value, continuation throughput,
directional MIS density and delta classification. Finite layered continuation
retains its sampled joint-path ratio while NEE and subsequent emissive hits use
the same fixed-stream approximate density. Domain-separated material-local
random streams preserve the outer sampler layout and absolute sample accounting.
This supports thin/diffuse transmission, finite coatings and tabulated scattering
without changing raw Film accumulation. See the [surface profile](pbrt-rendering.md#extended-surface-materials)
and [validation record](surface-material-validation.md) for the approximations,
resource limits and reference comparisons.

## Resumable execution

The progressive driver accepts validated committed Film state and resumes at its
uniform absolute sample count. Cancellation is observed between complete batches;
callbacks publish only committed state. A bounded portable checkpoint codec retains
all raw sums, compensation and counts. Canonical owned-scene identity and explicit
host/shader contracts permit continuation across compatible builds and supported
Metal devices, with per-session provenance and no cross-device bitwise claim.
See [checkpointing](checkpointing.md) for durability, identity exclusions, limits
and the fixed sample-target policy.

## Polygonal asset conversion

The portable OBJ adapter owns source-byte snapshots, authored polygon corners,
object/group/smoothing metadata and MTL fields separately from checked meshes.
Material-homogeneous parts target the existing position-indexed geometry; normal
synthesis precedes partitioning. Material conversion prepares existing PBRT
material graphs and bounded derived images without new GPU or transport types.

A separate publisher writes binary triangle PLYs, content-addressed images, a
PBRT world fragment and conversion provenance into a sibling staging directory.
Existing preflight and render translation validate the complete staged asset set
before a no-replacement directory rename. Relative references use a documented
bundle-root entry-scene convention. The [OBJ profile](obj-import.md) separates
preserved geometry, material approximations, and unsupported behavior.

The STL adapter independently owns ASCII/binary snapshots, solid ranges, original
facet normals and vendor attributes. Checked unwelded flat triangles are grouped
by effective diffuse color/coverage, retaining source-facet mappings. A named
sRGB/coverage profile maps vendor fields into existing PBRT material and alpha
parameters. It shares bounded PLY serialization and atomic directory publication
utilities with OBJ; each importer retains its own manifest and diagnostics.
Portable preparation checks generated-file and decode budgets before staging.
See the [STL profile](stl-import.md) for color ambiguity and numeric-domain rules.
Neither adapter changes transport, sampling, accumulation, or checkpoint contracts.

The static glTF adapter adds a separate checked scene-selection and resource
boundary using memory-only cgltf and bounded MikkTSpace. Core materials target
`rypath-gltf-v1`, with direct RGB image slots, native top-left UV sampling and
signed PLY frames; this introduces a dedicated spectral BSDF/coverage contract.
Nonemissive mesh reuse becomes object instances; emissive placements remain direct
geometry. Source snapshots, generated-file validation and atomic publication
follow the shared bundle model. See the [glTF profile](gltf-import.md) for limits,
selected-scene restrictions and explicit rendering adaptations.

## Rendering and planned boundaries

The first renderer exercises these boundaries concretely: portable spectral
sampling and compensated film accumulation, an owned native render session and
complete-path MSL kernel, a separate ImageIO PNG adapter, and a portable
OpenEXR publisher. A narrow native
accessor reuses the existing geometry owner's device, queue, TLAS, and BLAS
residency. Host code receives complete samples rather than individual bounce
queries. Predetermined batches commit only after checked GPU readback and full
sample accounting. The [rendering record](rendering.md) specifies the Lambertian
environment estimator, MIS/roulette, wavelength and sensor conventions, and
bounded ray-origin behavior.

PBRT Sobol, Halton and ZSobol sampling use shared CPU/MSL routines and pinned
direction/interval matrices or prime tables. Sampler settings
travel through the progressive driver and direct request boundary. Checked
absolute sample indices, fixed dimension assignments, and finite depth bounds
prevent table reuse or overflow. The demo and independent PBRT sampling retain
their original Philox sequence. Active perspective lenses append `lens-v1`:
Sobol/ZSobol dimensions 1022/1023, Halton dimensions 998/999, or a disjoint
independent Philox counter block, without
renumbering camera, wavelength, transport or dielectric draws. Inactive lenses
consume no lens draws. Current sampling contract 3, renderer contract 12 and identity
schema 10 reject earlier checkpoints globally. See
[sampling/defaults validation](sampling-defaults-validation.md),
[Sobol validation](pbrt-sobol-validation.md) and
[depth-of-field validation](depth-of-field-validation.md).

Positive diffuse area power is prepared per final generated primitive after
displacement. Portable preparation computes each shared image's native luminance
mean once, bounds the factor table, and uses rounded world geometry for triangle
and pinned PBRT bilinear normalization areas. Metal source-instance/kind mappings
select factors in the shared auxiliary buffer. Central emission evaluation applies
them to hits and both surface/volume NEE without changing proposals or PDFs;
legacy emission keeps an absent-factor sentinel. The power is part of canonical
identity. See the [rendering profile](pbrt-rendering.md).

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
respectively. The [attribute record](pbrt-attribute-validation.md)
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
- **Presentation:** PNG display output and linear HALF/FLOAT OpenEXR output are available.
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
durable checkpoint/resume, and cancellation. It also includes documented polygonal
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
