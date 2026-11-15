# PBRT spectral rendering, profile 1

`--render-pbrt` translates a checked PBRT scene into the native Metal renderer.
Structural [preflight](pbrt-profile.md) remains a separate portable operation;
successful parsing does not establish rendering compatibility.

```sh
cmake --preset release
cmake --build --preset release
./build/release/rypath --render-pbrt tests/fixtures/pbrt-render/multibounce.pbrt \
  --output build/pbrt.png --batch-size 8
```

The output path must end in `.png`; batch size defaults to eight. Resolution,
sample count, seed, and depth come from the scene. Progress reports committed
sample batches and only the completed image is published. The path overrides
`Film.filename`, and the terminal identifies the PNG/no-FP16 storage policy.
Exit 0 means rendering and publication succeeded, 2 means unsupported rendering
or unavailable Metal, and 1 means invalid input, resource, numeric, execution,
or output failure. Errors take precedence. Rejected scenes do not acquire a GPU
or change an existing output.

## Compatibility

Semantics are pinned to [PBRT v4 b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
The profile accepts effective parameters after preflight's shared typed
precedence/inheritance rules. Authored records and source locations remain
separate from narrowed render data. Unsupported effective definitions are
diagnosed even when not placed in the image.

| Component | Rendered behavior and limits |
| --- | --- |
| Descriptors | Explicit perspective camera, RGB film, box or triangle filter, independent or Sobol sampler, and path integrator; no substitution of omitted PBRT defaults |
| Camera | Pinhole, local +Z, PBRT raster orientation and smaller-axis FOV, aspect ratio/screen window; translation/rotation/reflection |
| Pose | Gram matrix and absolute determinant within `64 * FLT_EPSILON` of orthonormality; retain accepted basis without repair; scale/shear unsupported |
| Film | Full frame, CIE 1931 sensor, sRGB, positive ISO/shutter duration, white balance zero, no finite contribution clamp |
| Filter | Box radii 0.5 or triangle radii defaulting independently to 2 |
| Sampler | `independent` uses Philox4x32-10 with its own sequence; `sobol` uses the PBRT v4 interval sequence and supports all four Sobol randomizations |
| Integrator | Finite maxdepth 0–1024 with independent sampling and 0–127 with Sobol, default 5; regularization false; omitted or bvh light selection accepted, with rypath's explicitly different sampling distribution |
| Geometry | Checked inline triangles and PLY triangles/bilinear quads with per-vertex normals/UVs and single-level instances, including affine geometry transforms; at least one placed primitive |
| Materials | Two-sided diffuse with optional bump or RGB normal mapping, omitted reflectance 0.5, linear sRGB triplets in [0,1], inline numeric spectra with values in [0,1], continuous blackbody spectra, or built-in/file spectra clamped after interpolation |
| Textures | Typed float/spectrum graphs, PNG/JPEG imagemaps, core procedural textures, and UV/planar/spherical/cylindrical/3D mappings; see the bounded core profile below |
| Environment | Zero or one infinite light: a square RGB PNG/JPEG equal-area map or constant emission with nonnegative numeric or file spectrum, nonnegative linear sRGB emission, blackbody or built-in named emission, or omitted L using sRGB D65; scale defaults to 1, illuminance defaults to -1 |
| Area lighting | Directly placed diffuse triangle and bilinear patch emitters; numeric/file/RGB/blackbody/built-in named/default-D65 or RGB PNG/JPEG image emission, nonnegative scale (default 1), and twosided (default false); finite nonpositive power is inert, positive power unsupported |
| Acceleration | Omitted or untuned BVH descriptor maps to native Metal structures |
| Output metadata | Filename and FP16 setting explicitly overridden by PNG policy; no bitwise PBRT PNG equivalence |

Supported parameter defaults include film 1280×720, camera FOV 90, shutter [0,1],
ISO 100, seed zero, and depth five. Independent sampling defaults to four
samples per pixel. Sobol defaults to 16 samples per pixel and `fastowen`. Perspective
`focaldistance` is inert with zero lens radius; film `diagonal` is inert for this
camera. Exact full-frame crop/pixel bounds are accepted; other bounds are
unsupported. Float narrowing rejects overflow, collapsed spectral knots or
shutter intervals, and invalid packed camera coefficients.

Non-sRGB reconstruction, legacy `color` parameters, Ptex, floating-point/HDR image
inputs, fractional emitting alpha, emitting object members, other external mesh
formats, media, motion, lens effects, and other cameras, filters, samplers, and
integrators remain unsupported. The core texture boundary below lists the
additional exclusions.
Rendering requires Metal. The [unchanged CC0 Cornell fixture](../tests/fixtures/pbrt/README.md)
passes structural preflight and translates with its authored Sobol sampler,
1024×1024 film, triangle filter, 64 samples per pixel, and maximum depth 65.
The [render fixture manifest](../tests/fixtures/pbrt-render/README.md) records
the separately attributed Cornell adaptations used for numerical comparisons.

Triangle normals retain relative vertex lengths through affine transformation
and barycentric interpolation. The diffuse frame uses the shading normal; ray
offsets and area-light Jacobians use the geometric plane. Image reflectance uses
owned, position-indexed UVs. Constant reflectance and emission are UV-independent.
Degenerate one-sided emitter orientations and the supported numeric domain are
specified in [triangle-attribute validation](pbrt-attribute-validation.md).

## Constant textures

```pbrt
Texture "paint" "spectrum" "constant" "rgb value" [0.5 0.25 0.125]
Material "diffuse" "texture reflectance" "paint"
```

Inline and named diffuse materials may refer to a spectrum constant declared
anywhere in the scene world. Both constant texture types default to one. A
spectrum value may be linear sRGB, an inline numeric spectrum with at least two
increasing wavelengths and values in [0,1], blackbody temperature, or a built-in
name or spectrum file.
Named and file curves follow the diffuse contract below. An explicit
`rgb value` uses the `Texture` declaration's color space. An RGB value inherited
from `Attribute "texture"` uses the color space captured at that attribute.
Direct RGB reflectance continues to use the material dictionary's color space.

Translation checks every constant definition, including unused ones, before
rendering. All references to a spectrum constant share its compiled diffuse
material payload. Float constants accept signed values that narrow to finite
binary32. Composite graphs retain signed spectral operands until the final
material reflectance clamp. See the historical [constant texture validation
record](pbrt-texture-validation.md) and current core profile below.

## Blackbody reflectance

```pbrt
Material "diffuse" "blackbody reflectance" [3000]
Texture "paint" "spectrum" "constant" "blackbody value" [6500]
Material "diffuse" "texture reflectance" "paint"
```

Inline and named diffuse materials and spectrum constants accept a temperature
that narrows to positive finite binary32 kelvin. Effective inherited values and
shared texture references follow the same rules as other spectra, independently
of ColorSpace. Invalid values, including unused definitions, produce source-located
errors before GPU acquisition. Each effective temperature consumes one spectral
scalar; references share the compiled material.

At each path wavelength, evaluate the continuous Planck spectrum normalized at
PBRT's Wien peak, which may lie outside the visible interval, then clamp to [0,1].
This reflectance receives no light scale or CIE-Y calibration. CPU evaluation
uses double precision; Metal uses a stable float log-ratio and a bounded packed
exponent factor. The declared model is continuous, with separate CPU and device
numerical tolerances. Extremely small results and transport intermediates may
flush to zero on the device, so arbitrary HDR illumination does not establish
relative radiance accuracy for those tails. See the
[validation record](blackbody-reflectance-validation.md) for the exact contract.

## Named and file reflectance

```pbrt
Material "diffuse" "spectrum reflectance" "metal-Au-eta"
Texture "paint" "spectrum" "constant" "spectrum value" "paint.spd"
Material "diffuse" "texture reflectance" "paint"
```

Inline and named diffuse materials and spectrum constants accept all 88 pinned
built-in spectra or one checked spectrum file. Names take precedence over files.
These spectral inputs are independent of ColorSpace. Built-ins retain original
piecewise-linear knots, endpoint extension, and initialization normalization;
their integer-nanometer light tables are not used for reflectance. File curves
retain inclusive authored support and zero outside it, with no light calibration.

Finite signed binary32 file values, including values above one, are supported.
The diffuse model evaluates the raw curve and then clamps reflectance to [0,1],
matching pinned PBRT. Clamping authored ordinates before interpolation would
change the model. Zero and out-of-visible-range curves are valid black reflectance.
Existing inline numeric spectra retain their [0,1] input restriction.

CPU translation compiles bounded knots for the existing Metal material path.
Exact-product expansions preserve clamp crossings even across extreme signed
inputs. The compiler retains authored wavelengths and adds 360/830 nm anchors
and adjacent binary32 wavelengths around zero/one crossings. This preserves
evaluated reflectance at the renderer's binary32 path wavelengths in [360,830],
within the documented numerical tolerance. Between adjacent crossing neighbors,
or outside the visible interval, the compiled curve is not a general raw-spectrum
query interface. Checked file data and original built-in knots remain separate.
The unchanged Metal sampled-material path can flush tiny reflectance values or
interpolation products to zero. The absolute reflectance tolerance does not
guarantee relative radiance accuracy under arbitrarily intense illumination;
the validation record documents this observed numerical limit.

Source work and compiled storage each consume two spectral values per knot, in
addition to the authored string value. Both obey list and aggregate limits;
references to a constant texture share its compiled payload. See the
[reflectance validation record](spectral-reflectance-validation.md) for the
numerical contract and independent comparisons.

## PNG imagemaps

```pbrt
Texture "paint" "spectrum" "imagemap" "string filename" "paint.png"
Material "diffuse" "texture reflectance" "paint"
```

Inline and named diffuse materials may reference imagemaps on triangles and
bilinear quads, including object instances. Supported settings and defaults
follow the [pinned PBRT texture implementation](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/textures.cpp).

| Parameter | Supported values |
| --- | --- |
| `filename` | Required entry-relative or absolute path to a supported [PNG/JPEG input](pbrt-profile.md#png-imagemap-definitions) |
| `mapping` | `uv` (default), `planar`, `spherical`, or `cylindrical`; UV scales default to 1 and offsets to 0 |
| `encoding` | PNG: `sRGB` (default), `linear`, or `gamma` plus positive finite exponent; JPEG: fixed sRGB decode |
| `filter` | `point`, `bilinear` (default), `trilinear`, or elliptical `ewa`/`EWA` |
| `wrap` | `repeat` (default), `clamp`, or `black`, including non-power-of-two sources |
| `scale`, `invert` | Multiply filtered RGB by `scale` (default 1), then subtract from one when `invert` is true (default false) |
| `maxanisotropy` | EWA accepts 1–16, default 8; finite values remain inert with other filters |

Texture lookup maps the hit UV, flips the vertical coordinate and filters in
linear RGB. The spectrum uses the existing sRGB albedo reconstruction after
clamping the scaled or inverted RGB to [0,1]. Explicit texture encoding controls
sample decoding. PNG metadata gamma, color metadata, and alpha do not affect reflectance.
Float imagemaps, positive `gamma` encoding, non-UV mappings, alpha, surface detail,
and image emission follow the core texture profile below.

Indexed PNGs expand each palette index into its exact RGB8 entry, including
fully transparent entries. Palette alpha does not mask geometry or composite
against a background. Index depth controls packing, not color precision; these
inputs share the eight-bit mip and filtering path. See
[indexed PNG validation](indexed-texture-validation.md).

Packed 1-, 2-, and 4-bit grayscale expands to RGB8 before the selected transfer
function. All grayscale samples retain their value, including those matching a
transparency key. These sources share the eight-bit mip and filtering path; see
[grayscale validation](grayscale-texture-validation.md).

Eight-bit sources retain encoded-byte mip storage. Sixteen-bit samples are
normalized by `65535.f`, decoded with the selected transfer function, and rounded
to binary16 before resampling, following pinned PBRT. The sRGB path uses its
continuous transfer approximation rather than the eight-bit lookup table.
Resizing and downsampling propagate float working values; each stored level is
rounded independently to binary16. Resizing may produce values above one, which
remain available to filtering and texture scaling before the reflectance clamp.
Portable and Metal lookup widen stored halves to float, including half subnormals.
This texture storage policy is separate from film accumulation and PNG output's
`savefp16` override. See [16-bit validation](image16-texture-validation.md).

Point filtering uses the same floor LOD as bilinear, then rounds
`st * resolution - 0.5` with halfway values away from zero before applying
repeat, clamp, or black addressing. In particular, negative repeat coordinates
must not be normalized before rounding. The coarsest-LOD return is independent
of coordinates; a narrow footprint on a standalone 1×1 image still applies
wrapping. See [point validation](point-texture-validation.md) for signed seams,
bounded extreme-coordinate handling, and independent CPU/Metal evidence.

Trilinear filtering follows the pinned PBRT implementation: a selected mip index
of zero uses base-level bilinear filtering even at fractional LODs below one.
Higher fractional LODs blend two bilinear results in linear RGB. The upper 1×1
mip retains wrap handling during this blend; only LODs at or above the coarsest
level return its texel directly. Filter variants share the same mip storage.
See [trilinear validation](trilinear-texture-validation.md) for boundary and
independent reference checks.

EWA retains both signed footprint vectors, widens the shorter vector to respect
the anisotropy limit, and blends elliptical weighted averages at adjacent mip
levels. A zero shorter vector selects base-level bilinear. The actual 1×1 mip
still applies wrapping; only levels beyond it return the sole texel directly.
Black exterior taps contribute weight but zero RGB. The bounded anisotropy
profile and checked arithmetic prevent unbounded scans without truncating taps
or substituting another filter. See [EWA validation](ewa-texture-validation.md)
for numerical adaptations, coordinate limits, and reference evidence.

Triangle UVs default to `(0,0), (1,0), (1,1)`. Bilinear quads default to patch
parameters. The footprint uses the geometric tangent plane and the declared
sample count. Camera ray differentials supply primary footprints. Secondary
hits and primary projection fallbacks use PBRT's camera footprint approximation.
The [image texture validation record](image-texture-validation.md) describes
mip generation, the footprint calculation, and comparison coverage.

## Core texture graphs and consumers

Float and spectrum namespaces are distinct. All definitions, including unused
ones, are validated after parsing; forward references work, cycles and wrong
channel references are errors. Both channels support `constant`, `imagemap`,
`scale`, `mix`, `directionmix`, `bilerp`, `checkerboard`, and `dots`. Float graphs
also support `fbm`, `wrinkled`, and `windy`; spectrum graphs support `marble`.
The implementation follows the pinned factories, including their operand order,
conditional evaluation, image scale folding, and mapping derivative conventions.
Graph limits are 64 nodes of depth and 4,096 worst-case visits per evaluation.

Two-dimensional consumers support UV, planar, spherical, and cylindrical
mapping; three-dimensional checkerboard and noise use the inverse texture
declaration transform. Planar mappings transform position as well. Texture
positions are camera-relative with declaration inverses rebased before binary32
conversion. Filtered procedures use the existing primary/fallback footprint.
Signed numeric and file spectra stay unclipped inside graphs; diffuse albedo is
clamped only after evaluating the root. Image spectrum nodes retain PBRT's own
RGB albedo conversion before participating in a graph.

Float imagemaps reproduce PBRT's channel-selection differences: bilinear and
trilinear branches use Y, mean RGB, or retained nonopaque RGBA alpha; point,
nondegenerate EWA, and direct coarsest-mip lookup use channel zero. Degenerate EWA
uses its bilinear branch. Grayscale-alpha, palette transparency, and `tRNS` do
not provide scalar alpha. Geometry alpha is an explicit shape parameter.

Nonemitting triangle and bilinear shapes accept scalar alpha textures or values.
Values at most zero reject a candidate; values at least one accept it; fractional
values use the pinned packed-ray hash convention. Native queries preserve other
primitives at the same distance and every admissible patch root. All candidates
use zero texture derivatives. Hash tuples and rejected-root traversal can differ
from PBRT's recursive rays and transformed-primitive rays; sample identity with
PBRT is not claimed. Batch and scheduling changes preserve rypath's results.

Emitting alpha is restricted to resolved constants zero/one or direct UV point
imagemaps with unit scale, optional inversion, and exactly binary prepared base
texels and border. Certification happens after resize and encoding; an
uncertifiable emitting mask is rejected before GPU work. A constant-zero emitter
is invisible to all rays but supplies full NEE emission with weight one, following
PBRT's special case. An all-zero image mask is dark. Fractional emitting masks
are deferred because the pinned renderer applies different stochastic masks to
hit and NEE techniques without compensating its geometric MIS weights.

Diffuse `displacement` is bump mapping. `normalmap` uses an RGB native-resolution
PNG/JPEG with repeat bilinear sampling and takes precedence over bump mapping.
Triangle `vector3 S` tangents and normals form the declaration-space shading
frame before object placement. Surface detail changes the shading normal;
geometry, offsets, area measures, and emission sidedness keep geometric normals.
PNG normal maps decode as linear; pinned JPEG fallback decoding is always sRGB.

PLY shape `displacement` is true geometry displacement and requires vertex UVs.
Cyclic quads are triangulated, longest edges are refined in the shape-definition
metric with shared midpoints, displacement is evaluated in original object space,
and normals are rebuilt before Metal acceleration construction. Placement does
not trigger another refinement. Bounds are 64 refinement levels, 2²⁴ texture
node visits, the existing 2,000,000 scalar-value budget, and bounded scratch
storage. Other shape displacement remains unsupported.

Infinite image lights require a square RGB PNG/JPEG in PBRT's equal-area
octahedral projection and an orthonormal linear transform. Radiance uses native
nearest sampling with octahedral seams and calibrated RGBIlluminant/D65 spectra.
The proposal is 15/16 image-weighted sampling plus 1/16 uniform sphere sampling;
its PDF uses actual quantized CDF intervals and includes the uniform component.
The importance grid has at most 128 cells per axis; native texel mass is distributed
by area overlap into those cells, independently of native radiance resolution.
Black maps use a uniform CDF. BSDF escape and NEE use complementary power weights
with this PDF. Portals, multiple environments, positive area `power`, instanced
emitters, and authored bilinear emission-sampling controls remain deferred.

The [core texture validation record](core-texture-validation.md) distinguishes
pinned behavior, numerical adaptations, and measured evidence. APNG, EXR, PFM,
BMP, TGA, QOI, CMYK/YCCK JPEG, ICC color management, wider color spaces, Ptex,
and output/checkpoint/UI changes are outside this boundary.

## External PLY geometry

`Shape "plymesh" "string filename" "mesh.ply" loads the bounded
[PLY structural profile](pbrt-profile.md#ply-meshes). Quad faces remain bilinear
patches, including nonplanar geometry, bilinear corner normals and retained UVs.
Geometric patch parameters stay separate from UVs.
The numerical domain requires regular derivatives over the complete parameter
square and representable world-space reconstruction. World-coordinate ULPs and
the projected offset budget must remain below 1/1024 of the minimum tangent
altitude; uncertain transform arithmetic and excessive translations are rejected.
Singular or unresolved patches fail before GPU execution. This domain does not
promise visibility accuracy for arbitrarily close distinct surfaces.

Native triangle and procedural patch structures share the same scene-level
acceleration structure. Mixed meshes expand to separate native instances while
preserving source identities and graphics state. Accepted patch parameters travel
back from the intersection function in a ray payload; no second inverse-transform
intersection is needed.

Uniform square sampling of patch parameters uses the local differential-area
Jacobian for both light and BSDF-hit MIS weights. This proposal differs from
PBRT's optimized patch proposal while targeting the same integral. One-sided
attributed patch emitters require a certified sign of geometric/shading-normal
alignment across the complete patch; uncertain fields are diagnosed. Two-sided
emission does not need that sidedness restriction. See the
[PLY validation record](pbrt-ply-validation.md) for the estimator, bounds and evidence.

## Sobol sampling

`Sampler "sobol"` accepts a signed 32-bit seed and `none`, `permutedigits`,
`fastowen`, or `owen` randomization. Following the [pinned Sobol sampler](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/samplers.h),
rypath preserves a non-power-of-two sample count and warns at the authored
`pixelsamples` parameter. The CLI reports the sequence version, effective
randomization, and signed seed.

The PBRT interval mapping uses the absolute sample index and full-frame pixel
coordinates. Raw camera variates use the two unscrambled PBRT pixel dimensions
before reconstruction filtering. Wavelength and path variates use rypath's
fixed dimension assignments with the selected randomization. This establishes
the raw camera sequence and gives rypath a reproducible transport sequence
across progressive batches. PBRT's integrator consumes its own sequence of
dimensions and may therefore produce different paths for the same seed.

Sequence v1 assigns dimensions 0/1 to camera variates, 2 to wavelength, and
reserves 3. At zero-based bounce `b`, dimensions `4+8b` through `11+8b` supply
BSDF x/y, roulette, emitter slot, environment x/y, and area x/y, respectively.
Roulette compares the raw word with `0xc0000000`. Emitter selection takes the
high bits for its exact dyadic PMF; area coordinates retain the existing
high-bit midpoint grids. No dimension wrapping or independent-sampler tail is used.

Let `m = ceil(log2(max(width, height)))`. Sobol requires `m <= 25` and each
absolute sample index to be less than `2^(52 - 2m)`. The finite Sobol depth
limit of 127 keeps all path dimensions within the available Sobol table.
Translation checks these limits at the film, sampler, or integrator declaration
before acquiring a device.
The matrix bound does not promise subpixel precision at extreme resolutions:
PBRT's binary32 global-coordinate remapping loses local fraction bits at large
pixel coordinates. Filtering can introduce further rounding differences between
renderers. See the [Sobol validation record](pbrt-sobol-validation.md), and
[PLY validation record](pbrt-ply-validation.md).

## Pixel reconstruction

Triangle filtering samples the normalized separable tent around each pixel center.
Each radius must narrow to a positive normal binary32 value. Invalid radii and
unrepresentable camera arithmetic produce source-located errors before device
acquisition. Radii may exceed film dimensions when the camera arithmetic remains
supported; the renderer applies no arbitrary radius cap.

Samples retain their originating pixel and unit filter weight, including when
positions lie outside the image. There is no clipping, splatting, edge
renormalization, or additional radius factor. The selected sampler supplies the
two camera uniforms independently of wavelength and transport dimensions.
The finite uniform grids include zero and exclude one. Philox's sequence differs
from PBRT; Sobol's raw camera variates match the pinned sampler before filtering.
The [filter validation record](pbrt-filter-validation.md)
describes inverse sampling, float bounds, and independent acceptance tests.
Half-pixel box rendering and the demo retain their original camera arithmetic.
The terminal reports the effective filter and both radii. No new CLI option is
needed; use `PixelFilter "triangle" "float xradius" [1.5] "float yradius" [.75]`
in the scene, or omit either radius to use its default of two.

## Spectral and transport contract

Numeric materials retain their authored knots, narrowed to float after validation.
Piecewise-linear values are zero outside the supplied interval; materials are
not projected onto the demo's eight knots or a one-nanometer grid. Storage and
expansion work remain bounded by the preflight value budget.

For an explicit numeric infinite-light spectrum, evaluate S at integer wavelengths
360 through 830 nm, and compute `D = sum(ybar(n) * S(n))`. Require finite positive D. The radiance
table is `S(n) * scale / D`, additionally multiplied by `illuminance / pi` when
illuminance is positive. Zero or negative illuminance applies no additional
multiplier. There is no factor of 683. Light and observer evaluation
select the nearest integer nanometer, with half-way values rounded upward,
following the pinned dense spectra.

Named emission accepts one case-sensitive built-in name, for example
`"spectrum L" "stdillum-F11"`. All 88 names at the pinned PBRT revision are
available: 16 illuminants, 21 glass/metal optical spectra, and 51 camera-response
spectra. Using an optical or response curve for emission follows PBRT's general
named-spectrum lookup; it does not enable those materials or film sensors.
Preflight and rendering share one registry. Unrecognized names retain their
entry-relative filename interpretation and existing dependency diagnostics.

The immutable 471-sample tables preserve PBRT's binary32 interpolation, constant
endpoint extension at 359/831 nm where required, and initialization normalization
of illuminant knots. Explicit `stdillum-D65` is identical to the existing D65
table. Named emission is independent of ColorSpace and uses its own CIE-Y inner
product with the numeric-emission calibration above; it receives no RGB tint,
reflectance clamp, or additional factor of 683. Checked double-precision scaling
preserves finite final radiance even when an intermediate PBRT float scale would
overflow. Final binary32 overflow is still a source-located error, including
unused area declarations. Named diffuse and constant-texture reflectance use the
separate [original-knot path](#named-and-file-reflectance). See
[named-spectrum validation](named-spectrum-validation.md) for emission evidence.

File emission accepts `"spectrum L" "lights/key.spd"` or a single bracketed
filename. Preflight resolves and retains the checked binary32 knots under the
[file input profile](pbrt-profile.md#spectrum-files). Translation uses
the same double-precision interpolation as numeric sampled emission, then packs
the 360–830 nm table and applies the calibration above. Values are zero outside
the inclusive authored wavelength interval; file curves do not receive built-in
spectra's constant endpoint extension. Negative values are unsupported, and an
all-zero or out-of-range curve with zero CIE-Y sum is an error even at scale zero.
Normal IEEE rounding applies when interpolation and calibration narrow to the
dense table. This is a numerical adaptation of PBRT's binary32 interpolation,
not a bitwise equivalence claim. See [file-spectrum validation](file-spectrum-validation.md).

Blackbody light emission accepts `"blackbody L" [3000]`, with temperature in
kelvin. The authored temperature must narrow to a positive finite binary32
value; overflow and positive values rounding to zero are source-located errors.
Explicit and inherited blackbody values are independent of ColorSpace.
Planck's spectrum uses the pinned PBRT physical constants and the same CIE-Y
normalization as numeric emission. There is no factor of 683 or D65 tint.
The peak normalization of PBRT's `BlackbodySpectrum` cancels in this light
calibration. rypath evaluates an equivalent ratio to the 830 nm value with
double-precision `expm1` and nonpositive exponentials, retaining double samples
through normalization before narrowing the final radiance table. This avoids
overflow and premature spectral underflow at extreme temperatures; final
binary32 radiance overflow remains an error, even on an unused area declaration.
Scale zero yields exact black for an otherwise valid temperature.

This is a numerical adaptation of PBRT's model, not bitwise agreement with its
binary32 `FastExp` implementation at arbitrary temperatures. At the cold limit,
only the 830 nm table entry survives. The discrete CIE-Y calibration counts it
fully, but uniform wavelength sampling over [360,830] gives the endpoint bin
half width. Thus raw film Y approaches half the calibrated amplitude in that
limit. The dense model and wavelength estimator remain unchanged. See
[blackbody validation](blackbody-light-validation.md) for independent checks and
the moderate-temperature pinned PBRT comparisons.

Linear sRGB diffuse colors use the pinned PBRT/Jakob–Hanika sigmoid polynomial,
evaluated continuously at the sampled wavelength. RGB is not display encoded and
receives no transfer-function decoding. Neutral RGB becomes a constant spectrum;
this preserves the intended gray value, including exact black/white, without
PBRT's infinite endpoint coefficients or tiny-gray overflow. This is a numerical
adaptation, not a claim of bitwise equivalence at extreme inputs. The canonical
64³ reconstruction table is embedded; see its [provenance](../third_party/pbrt/README.md).

For RGB light emission, let `m = max(R,G,B)`. For nonzero m, reconstruct
`RGB / (2*m)`, multiply by `2*m` and D65, then evaluate at integer nanometers.
RGB black is exactly zero. The normalization denominator is the **base D65**
CIE-Y sum, not the tinted emission's sum. Omitted L uses D65 with that same
denominator. Existing scale/positive-illuminance factors apply afterward.
Binary32 input or reconstruction-scale overflow is an error even if a later
normalization might reduce the result. Nothing clamps HDR emission into range.

Diffuse RGB uses the material declaration's color space even for an inherited
parameter. Explicit RGB L uses the effective parameter's captured color space;
omitted L uses the light declaration's space. Only the consumed space must be
sRGB. Numeric spectra remain independent of ColorSpace. These context-specific
rules follow the [pinned parameter implementation](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/paramdict.cpp).

Each path retains one uniformly sampled wavelength with density 1/470. Raw
unexposed XYZ receives `470 * L(lambda) * CIE_nearest(lambda)`, without the
demo's Y-reference division. Film projection applies
`(shutterclose - shutteropen) * ISO / 100`, then the linear sRGB matrix. Display
conversion alone applies sRGB encoding, clipping, and eight-bit quantization.
Raw compensated double XYZ and counts remain independent of display. The
existing CIE table matches the pinned PBRT values. RGB reconstruction extends the
material/light model. Area lighting adds its own proposal and complementary
emissive-hit MIS; wavelength sampling and roulette are unchanged.

The [existing Lambertian NEE/MIS and compensated roulette](rendering.md) operate
on imported spectra. Escape and sided surface emission precede the depth cutoff.
At an opaque hit, the authored cutoff precedes another direct-light estimate or
scattering, and the emergency watchdog. Depth zero preserves visible sky and
emission; non-emitting opaque hits are black. This targets the finite-depth integral, not infinite-depth transport.
PBRT uses different wavelength/path sampling, roulette, and environment
sampling; comparisons are statistical rather than sample-by-sample.

Watchdog and nonfinite failures reject the entire pending batch, rather than
successfully truncating or discarding samples. High reflectance and large depth
can produce high variance with fixed-probability roulette. No performance or
arbitrary-scale geometry guarantee is made. The demo retains its unit
environment, relative observer response, and compensated stopping policy.

Area emission uses the same dense spectral calibration as the environment,
without an illuminance multiplier. Emission and reflectance are independent;
black diffuse emitters remain visible. The emission normal preserves winding,
transformed handedness, and ReverseOrientation before face-forwarding the
scattering normal. Two-sided emission changes the emitting hemisphere, not its
radiance. An empty image filename is inert. An RGB PNG/JPEG filename uses native
resolution, clamp-wrapped bilinear sampling at `(u, 1-v)`, and RGBIlluminant/D65
spectral reconstruction. Combining a nonempty filename with effective `L` is
invalid. Image emitters retain the existing geometric proposal and MIS density.
The authored finite value determines whether power is positive; this deferred
parameter is not narrowed to float. Transformed emitters use checked world-space
geometry for both native acceleration and shading, while preserving source
instance/primitive ordering and effective orientation. The authored scene stays
unchanged.

Each scattering vertex draws one area sample separately from the existing
environment sample. Real primitives occupy N of M=bit_ceil(N) equally likely
slots; unused slots contribute zero. The area sample and BSDF-hit emission use
complementary power weights including the exact 1/M selection probability.
Triangle samples are uniform in area; patch samples are uniform in parameter
space with reciprocal area density equal to the local Jacobian. These proposals
differ from PBRT's BVH light selection and directional shape sampling. Finite visibility rays offset both endpoints and
retain blockers before the light. Numerical limits, PDFs, and fixed validation
criteria are recorded in [area-light validation](pbrt-area-validation.md).

Patch validation is bounded before expansion: at most 2,000,000 patch corners
across placements, additionally capped by the configured PBRT total-value limit.
Repeated object placement cannot multiply derivative validation without bound.

## Reference and validation

Routine CTests remain offline and include portable translation/camera/spectral
checks, native analytic checks, CLI failure/output behavior, and existing
geometry/demo regressions. Native tests require a GPU unless hosted CI explicitly
permits an observed missing capability; execution failures never skip.

The opt-in reference is a CPU Release/float build of the pinned PBRT revision
and exact submodules under `.cache/pbrt-reference`; it is not linked into rypath.
One canonical sRGB table is separately vendored as licensed renderer data; the
remaining reference build assets stay in the cache. Record toolchain,
dependency versions, source/submodule identities, and binary hash with each
comparison. Commands and measured evidence are in the
[spectral validation record](pbrt-render-validation.md),
[sRGB validation record](pbrt-rgb-validation.md),
[area-light validation record](pbrt-area-validation.md),
[triangle-filter validation record](pbrt-filter-validation.md),
[triangle-attribute validation record](pbrt-attribute-validation.md),
[Sobol validation record](pbrt-sobol-validation.md),
[PLY validation record](pbrt-ply-validation.md),
[constant texture validation record](pbrt-texture-validation.md),
[image texture validation record](image-texture-validation.md),
[blackbody validation record](blackbody-light-validation.md),
[named-spectrum validation record](named-spectrum-validation.md),
[file-spectrum validation record](file-spectrum-validation.md),
[spectral reflectance validation](spectral-reflectance-validation.md), and
[blackbody reflectance validation](blackbody-reflectance-validation.md).

Owned wrappers and five labeled CC0 Cornell derivatives cover
numeric-spectrum, file, RGB, blackbody, and built-in named lighting, diffuse response, area emission, visibility,
asymmetric multiple scattering, transformed shading normals, randomized
Sobol sampling, mixed PLY primitives, curved emission, and named constant and PNG
image textures. The external PLY and PNG inputs retain their own licenses.
Comparison uses 8×8 pixels, 4,096 samples per pixel, and
16 fixed seeds per renderer (rypath 0–15 and PBRT 16–31). Each channel and
declared region mean must agree within five combined standard errors plus
`2e-5 * max(1, abs(reference))`. Thresholds and seeds precede results; failures
are investigated without retries chosen to pass. For the three randomized Sobol
modes, these errors are conditional on the fixed unscrambled camera sequence.

Shared inputs set `savefp16=false`. PBRT writes linear binary32 PFM through its
output override. The test-only rypath harness uses the production translator,
Metal renderer, and film projection to expose unquantized linear RGB. The PFM
reader checks dimensions, endianness, byte count, and bottom-up row order while
preserving negative and above-one values. Product output remains PNG.

```sh
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir .cache/pbrt-reference/comparison
```
