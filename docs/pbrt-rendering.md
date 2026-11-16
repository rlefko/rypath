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

The output path must end in `.png` or `.exr`; batch size defaults to eight. Resolution,
sample count, seed, and depth come from the scene. Progress reports committed
sample batches and only the completed image is published. The path overrides
`Film.filename`. PNG uses the existing display-sRGB conversion and overrides
`Film.savefp16`. EXR publishes linear sRGB with ZIP compression and honors
`Film.savefp16`: omitted or true selects HALF; false selects FLOAT.
The terminal reports the format, precision and actual overrides. EXR projection
uses compensated XYZ means and sensor exposure without display clipping or
transfer encoding. Negative and above-one finite values survive; nonfinite
values and HALF/FLOAT overflow fail. Publication replaces the destination only
after encoding, finalization and close succeed. The
[HDR record](hdr-image-validation.md) specifies metadata and validation.
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
| Descriptors | Perspective camera, RGB film, box/triangle/Gaussian filter, independent/Sobol/Halton/ZSobol sampler, and path/volpath integrator; omitted descriptors use pinned PBRT defaults |
| Camera | Pinhole or perspective thin lens, local +Z, PBRT raster orientation and smaller-axis FOV, aspect ratio/screen window; translation/rotation/reflection |
| Pose | Gram matrix and absolute determinant within `64 * FLT_EPSILON` of orthonormality; retain accepted basis without repair; scale/shear unsupported |
| Film | Full frame, CIE 1931 sensor, sRGB, positive ISO/shutter duration, white balance zero, no finite contribution clamp |
| Filter | Positive normal binary32 box radii defaulting to 0.5, triangle radii to 2, or tabulated Gaussian radii 1.5 and sigma 0.5 |
| Sampler | Philox independent, PBRT interval Sobol, Halton, or ZSobol with pinned randomizations and versioned rypath transport dimensions |
| Integrator | Path for vacuum surface transport or volpath for homogeneous media; finite maxdepth 0–1024 with independent sampling, 0–127 with Sobol/ZSobol (0–113 with dielectrics), or 0–124 with Halton (0–110 with dielectrics), default 5; regularization false; omitted or bvh light selection accepted, with rypath's explicitly different sampling distribution |
| Geometry | Checked inline triangles/bilinear meshes, Loop subdivision triangles, and PLY triangles/bilinear quads with supported vertex attributes and single-level instances, including affine geometry transforms; at least one placed primitive for path; volpath permits empty geometry |
| Materials | Two-sided diffuse with optional bump or RGB normal mapping, omitted reflectance 0.5, linear sRGB triplets in [0,1], inline numeric spectra with values in [0,1], continuous blackbody spectra, or built-in/file spectra clamped after interpolation |
| Conductors | Spectral complex-IOR Fresnel with anisotropic single-scattering GGX or smooth reflection; raw named/numeric/file eta/k, independent scalar roughness graphs, bump/normal mapping; see [conductor profile](#spectral-ggx-conductors) |
| Dielectrics | Smooth and anisotropic single-scattering GGX reflection/transmission, scalar or spectral eta, textured roughness and mapped frames; see [dielectric profile](#spectral-ggx-dielectrics) |
| Extended surfaces | Thin dielectric, diffuse transmission, nested mixes, finite-depth coated diffuse/conductor, measured tensor and legacy Fourier tables; see [surface materials](#extended-surface-materials) |
| Textures | Typed float/spectrum graphs, PNG/JPEG/EXR/HDR imagemaps, core procedural textures, and UV/planar/spherical/cylindrical/3D mappings; see the bounded core profile below |
| Environment | Zero or one infinite light: a square RGB PNG/JPEG/EXR/HDR equal-area map or constant emission with nonnegative numeric or file spectrum, nonnegative linear sRGB emission, blackbody or built-in named emission, or omitted L using sRGB D65; scale defaults to 1, illuminance defaults to -1 |
| Point lighting | Up to 64 combined point/spot/goniometric/projection sources; isotropic point lights with spectral `I`, nonnegative `scale`, optional `power` and transformed `from`; direct surface and homogeneous-volume illumination |
| Projection lighting | Native rectangular RGB images, nearest perspective lookup, affine scale/shear, optional photometric power, and direct surface/homogeneous-volume illumination |
| Spot lighting | Spectral point-source emission with cubic cosine falloff, `from`/`to` aiming, representable static affine transforms and positive-power normalization; surface and homogeneous-volume illumination |
| Goniometric lighting | Spectral `I`, `scale`, optional `power`, required native square scalar image diagram, rigid/reflected declaration transforms; surface and homogeneous-volume illumination with nearest equal-area lookup |
| Distant lighting | Up to 64 directional sources with spectral `L`, `scale`, `illuminance` and affine `from`/`to`; finite PBRT scene-bound visibility for surfaces and homogeneous media |
| Area lighting | Directly placed diffuse triangle and bilinear patch emitters; numeric/file/RGB/blackbody/built-in named/default-D65 or RGB PNG/JPEG/EXR/HDR image emission, nonnegative scale (default 1), and twosided (default false); finite nonpositive power is inert, positive power normalizes each generated primitive |
| Homogeneous media | Named absorption/scattering/emission spectra, all 47 pinned measured presets, scoped interfaces and null boundaries; see [homogeneous media](#homogeneous-media) |
| Acceleration | Omitted or untuned BVH descriptor maps to native Metal structures |
| Output | Explicit filename override; PNG display encoding or linear EXR with authored/default HALF or FLOAT precision; no bitwise PBRT image equivalence |

Supported parameter defaults include film 1280×720, camera FOV 90, shutter [0,1],
ISO 100, seed zero, and depth five. Independent sampling defaults to four
samples per pixel. Sobol defaults to 16 samples per pixel and `fastowen`. Perspective
`focaldistance` is inert with zero lens radius; film `diagonal` is inert for this
camera. Exact full-frame crop/pixel bounds are accepted; other bounds are
unsupported. Float narrowing rejects overflow, collapsed spectral knots or
shutter intervals, and invalid packed camera coefficients.

Non-sRGB reconstruction, legacy `color` parameters, Ptex, fractional emitting alpha, emitting object members, other external mesh
formats, heterogeneous media, motion, realistic lens systems, and other cameras, filters, samplers, and
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

## Omitted descriptors

Missing camera, film, filter, sampler and integrator declarations resolve to
perspective, RGB, Gaussian, ZSobol and volpath respectively. Explicit unsupported
types remain unsupported. Defaults are effective render data; structural
preflight retains only authored descriptors. An omitted camera stays at identity
in vacuum and does not define the named `camera` coordinate system, regardless
of prior option transforms or medium state. The implicit RGB film starts in sRGB.
Each applied default is recorded in render provenance.

## Perspective depth of field

`Camera "perspective" "float lensradius" [.1] "float focaldistance" [4]`
enables a circular thin lens. Radius defaults to zero (the existing pinhole);
focal distance defaults to `1e6` and measures axial distance along camera +Z.
Both positive lens parameters and their ratio must remain normal binary32
values. Zero radius leaves focal distance inert, including its scene identity.
Camera scale/shear and animation remain unsupported.

Uniform concentric-disk samples originate on the aperture and target the
corresponding point on the focal plane. The normalized aperture integral has
unit camera weight, following PBRT perspective-camera semantics; aperture size
does not multiply brightness. This model does not simulate lens elements,
diffraction, aberrations or polygonal aperture blades. The camera's authored
medium applies to every aperture point; no geometric medium classification is
performed at the lens.

Lens draws use Sobol dimensions 1022/1023 or Philox counter block `0xc0000000`,
without shifting existing camera, wavelength or transport draws. All supported
Sobol randomizations and existing depth limits remain available. Primary and
neighboring differential rays reuse one aperture point. Texture mapping remains
camera-centered; primary derivatives use the aperture-relative hit point. The
existing camera-based approximation for indirect footprints remains unchanged.

The aperture is quantized at the binary32 world origin. Directions and primary
derivatives use that realized displacement consistently. Preflight and native
requests bound complete filter/aperture support and reject overflow, collapsed
aperture axes or potentially vanishing directions. They do not discard individual
samples or silently substitute a pinhole. The
[depth-of-field validation record](depth-of-field-validation.md) describes the
numerical domain, regression tests and matched reference comparisons.

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

## Point lights

`LightSource "point"` accepts `I` in the existing RGB, blackbody, numeric,
built-in and checked-file emission profiles. Omitted `I` uses the declaration's
color-space illuminant (currently sRGB D65). `scale` defaults to one. Intensity
uses the same pinned photometric preparation as other spectral lights, including
RGB base-illuminant normalization. Positive `power` additionally multiplies by
`power / (4 * pi)`; finite nonpositive power retains intensity mode.

The declaration transform acts on `point3 from`, which defaults to the origin.
Scaling or shearing the transform moves that position without changing isotropic
intensity. Light parameters inherit from `Attribute "light"`; definitions inside
objects remain global rather than instanced. Nonempty endpoint medium names
must resolve, but direct-light connections use the receiver's medium and crossed
boundaries. A light's endpoint medium alone does not require `volpath`.

At each eligible scattering vertex, rypath sums all point sources with
`I(lambda) / distance_squared`, BSDF times receiver cosine or HG phase, and
visibility/transmittance. Delta-position contributions have unit MIS weight;
they do not change environment/area probabilities or consume new sampler
dimensions. Delta-only surfaces have no point next-event estimate. Point lights
have no visible geometry and add no camera-hit or escaped-ray emission.

Finite shadow rays offset the receiver but end at the point's position, without
an emitter-normal offset. Alpha, null boundaries and homogeneous attenuation
follow existing visibility rules; ordinary glass remains a shadow occluder.
Geometry-free homogeneous media use the same point estimator with analytic
Beer–Lambert connections in the sky kernel.

The declared profile permits at most 64 combined point, spot, goniometric and projection
sources, including zero-intensity records;
excess inputs are resource errors. Active connections require finite squared
distance at least `FLT_MIN`, representable arithmetic and positive shadow progress.
Coincident active sources and extreme-scale failures reject the pending batch;
there is no distance epsilon, intensity clamp or partial sample acceptance.
Zero intensity at the sampled wavelength skips connection arithmetic. The
all-positional strategy can require 64 shadow queries per vertex and is not a claim
of efficient many-light sampling. See [point-light validation](point-light-validation.md).

## Spot lights

`LightSource "spot"` shares the point light's spectral `I`, `scale`, `power`,
parameter inheritance, endpoint-medium and finite visibility behavior. `from`
defaults to (0,0,0), `to` to (0,0,1), `coneangle` to 30 degrees and
`conedeltaangle` to 5 degrees. Both points are in declaration-local coordinates.
The supported angle domain is `0 < coneangle <= 180` and
`0 <= conedeltaangle <= coneangle`; effective `from` and `to` must differ.
Defaults apply before these checks, so a cone smaller than five degrees needs
an explicit smaller delta. Invalid pairs are source-located input errors.

Following the [pinned implementation](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp),
`coneangle` is the outer boundary; full intensity begins at
`coneangle - conedeltaangle`. Let `cEnd` and `cStart` be their binary32 cosines.
Between them, `t = (cosTheta - cEnd) / (cStart - cEnd)` and the angular factor
is `t*t*(3-2*t)`, with zero outside and one inside. Zero delta is a hard edge
that includes equality. A 180-degree hard cone is isotropic.
The format page's description of the angle interval differs from this pinned
code; this profile follows the code.

Positive `power` multiplies photometrically prepared intensity by
`power / (2*pi*((1-cStart)+(cStart-cEnd)/2))`. Finite nonpositive power retains
intensity mode. The divisor uses the packed cosine thresholds, an explicit
binary32 numerical adaptation. A cone with `cEnd == 1`, or a positive feather
whose cosines coincide, is rejected instead of silently becoming a zero-width
cone or hard edge. Native requests also reject subnormal cosine thresholds or
positive feather intervals that GPU subnormal flushing would erase.

The receiver-to-light direction is negated, mapped through the inverse composed
light transform, and normalized before taking its local Z component. This
preserves the pinned behavior under rotation, reflection, nonuniform scale and
shear. A common positive matrix scale bounds GPU coefficients without changing
the ideal normalized direction. Translation and native request validation reject
nonfinite, singular or unrepresentable maps, including coefficients that would
be lost to GPU subnormal flushing. Spot position coordinates must be normal
binary32 or zero. Active connections also check receiver coordinates, angular
products, sums and normalization divisions: subnormal flushing must not turn a
negative cosine into an included hard-cone boundary. Unrepresentable connections
reject the complete batch. Transformed directions use scale-safe normalization.
This is a bounded floating-point profile, not arbitrary-scale exact arithmetic.
PBRT's power normalization is retained under affine distortion; requested power
is not guaranteed to equal integrated world-space flux after nonuniform scale
or shear.

At every eligible surface or medium vertex, each spot contributes its angular
intensity divided by squared world distance with the existing BSDF/cosine or
HG phase and shadow transmission. Sources are enumerated with unit delta-position
MIS weight, without new sampler dimensions or changes to environment/area PDFs.
Angular zero skips shadow traversal after safe direction evaluation. Spot lights
have no camera-visible geometry, and delta-only surfaces receive no spot NEE.
Geometry-free homogeneous media execute the same model in the sky kernel.

Point, spot, goniometric and projection lists share the 64-source budget. Zero spectral
intensity skips connection arithmetic; an active coincident source fails the batch. All spot
fields participate in checkpoint identity. Spot lighting introduced renderer
and identity contracts 4; [checkpointing](checkpointing.md) records the current
versions. See [spot-light validation](spot-light-validation.md).

## Goniometric lights

`LightSource "goniometric"` accepts the same spectral `I` forms and photometric
preparation as point lights, with omitted `I` using D65, nonnegative `scale`
(default one), and finite `power` (default -1). It requires a nonempty square
`string filename` in the existing bounded PNG/JPEG/OpenEXR/Radiance HDR profiles.
Supported decoded gray, RGB and RGBA inputs become a native-resolution scalar
map; alpha does not modulate emission. The declaration origin supplies position;
`from`, `to` and spot cone parameters are outside this schema. Effective light
parameters inherit and override normally, checked files remain owned, and lights
inside object definitions remain global rather than instanced.

Gray values preserve their source representation after the existing decode.
RGB/RGBA conversion follows the pinned PBRT code: decode linear components,
sum and divide by three in binary32, then write the scalar through the source
image format and encoding. This quantizes byte RGB averages back through their
encoding; HALF averages round back to HALF, and FLOAT averages remain binary32.
For 16-bit PNG RGB, each decoded component first rounds to HALF and the scalar
average rounds to HALF again. Gray inputs avoid that averaging/write round trip.
The retained map contains one replicated scalar channel, no mip pyramid or
power-of-two resize. Negative or nonfinite source color/scalar values, source
sum or format overflow, and binary32 subnormal scalars are rejected. These are
explicit numerical restrictions on the broader PBRT input model; alpha is
ignored rather than averaged into the diagram.

Following the [pinned light implementation](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp),
negate the unit receiver-to-light direction, apply the actual inverse declaration
linear transform, then swap Y and Z. Feed that result directly to PBRT's
polynomial equal-area sphere-to-square map and nearest-texel lookup with clamp
boundaries. There is no additional V flip, bilinear filtering, direction
renormalization or common matrix rescaling. Nearest-texel boundaries make these
operations observably different even for nearly orthonormal transforms.

The declaration linear transform must be orthonormal within absolute Gram-entry
error `64 * FLT_EPSILON`; rigid transforms and reflections are supported.
Scale or shear beyond that tolerance is unsupported. Translation computes the
checked actual inverse, including accepted near-tolerance deviations, and rejects
singularity, overflow or coefficients lost in binary32. The direct scene API
validates the supplied inverse map at `128 * FLT_EPSILON` to accommodate inversion
and binary32 narrowing. Positions, intensities and inverse coefficients must be
finite normal binary32 or zero. Unrepresentable runtime angular arithmetic fails
the complete pending batch.

Finite nonpositive `power` leaves intensity mode unchanged. Positive power
multiplies photometrically prepared intensity by
`power / (4 * pi * mean_prepared_scalar)`, in addition to `scale`. The mean uses
the decoded retained scalar samples accumulated in binary64. This widened sum
is a numerical adaptation that avoids binary32 accumulation overflow; it does
not reproduce PBRT's exact binary32 summation rounding. Positive power requires
a positive finite mean and representable normalization. A black map is valid
without positive power, and zero spectral intensity remains black.

At every eligible surface or medium vertex, rypath sums all positional sources;
goniometric intensity receives the scalar angular factor before inverse-square
attenuation, the existing BSDF/cosine or HG phase, and finite visibility and
homogeneous transmittance. Geometry-free homogeneous media use the same model
in the sky kernel. These delta-position terms have unit MIS weight, consume no
new sampler dimensions, and leave environment/area probabilities unchanged.
There is no visible source geometry, and delta-only surfaces receive no
next-event term. Point, spot, goniometric and projection sources share the 64-source limit,
including black records. Existing finite-connection failures reject the whole
batch. Ordered positions, every spectral bin, inverse maps, image bindings and
owned scalar texels participate in checkpoint identity. See
[checkpointing](checkpointing.md) and the
[goniometric validation record](goniometric-light-validation.md).

## Projection lights

`LightSource "projection"` requires a nonempty RGB/RGBA `filename` in the bounded
PNG/JPEG/OpenEXR/Radiance HDR profiles. Native dimensions may be rectangular or
non-power-of-two; there is no mip pyramid or resize. Alpha is ignored. Image
color uses the existing sRGB/D65 profile and RGBIlluminant spectral reconstruction.
There is no separate `I` or `L` spectrum, nor `from`/`to` parameter. The declaration
origin supplies position. Parameters inherit from `Attribute "light"`; checked
images remain owned, and lights declared inside objects remain global.

`scale` is nonnegative and defaults to one. Finite `power` defaults to -1;
nonpositive power retains scale mode. `fov` defaults to 90 degrees and controls
the shorter image dimension. Both authored and narrowed FOV must lie strictly
between 0 and 180 degrees, with positive normal binary32 projection coefficients.
For aspect `a = width / height`, the symmetric screen half-extents are `(a,1)`
when wide and `(1,1/a)` otherwise.

The [pinned PBRT implementation](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp)
appends a local Y reflection to the declaration transform. rypath applies that
actual inverse to the unit world light-to-receiver direction, without direction
normalization or common matrix rescaling. Rotation, reflection, nonuniform scale
and shear are supported. Coefficients and position must survive binary32 packing
as finite normal values or exact zero; singularity after narrowing is rejected.
There is no arbitrary scale or condition-number cutoff, but active connections
must remain representable.

A local direction with `z < 0.001` contributes zero. This is an angular test on
the actual inverse-transformed direction, not a world-space distance cutoff.
For example, an on-axis declaration Z scale of 500 passes, while 2000 is dark.
Perspective multiplies X/Y by the inverse half-FOV tangent before division by Z.
Inclusive screen bounds become UV offsets with no extra V flip. Native nearest
lookup clamps UV endpoints to the last texel; it does not use bilinear filtering.

Scale is divided by D65 photometry. Positive power additionally multiplies by
`power / (A * mean(luminance(texel) * cos(theta)^3))`, evaluated at light-space texel centers,
where `A = 4 * tan(fov/2)^2 * max(a, 1/a)`. The sum and derived normalization use
binary64, a numerical adaptation from PBRT's binary32 accumulation. A positive
power requires a positive finite integral, even for zero scale. Normalization
retains PBRT's light-space definition without an affine Jacobian; scale or shear
can change integrated world-space flux despite a fixed authored power.

Projection images reject negative, nonfinite and positive binary32-subnormal
RGB, and values that overflow RGBIlluminant reconstruction. This is narrower
than PBRT's negative-channel clamping. Runtime checks cover inverse mapping,
projection, normalized RGB divisions and reconstructed spectral products.
Nonfinite or lost nonzero intermediates fail the pending batch without partial
Film accumulation; they do not silently become black or out-of-bounds samples.
Zero source scale skips connection arithmetic. Black maps are valid without
positive power. Other image consumers retain their established numerical policy.

Projection records share the combined 64-source bound, finite endpoint visibility,
alpha/null handling and delta-position estimator with point/spot/goniometric
lights. Surface and homogeneous-medium vertices enumerate every source with
unit MIS weight and no additional sampler dimensions. Geometry-free media bind
the same image resources. Projection lights have no camera-visible geometry.
All effective records and owned images participate in scene identity. Projection
support introduced renderer contract 8 and identity schema 7; see
[projection validation](projection-light-validation.md) and the
[current checkpoint contracts](checkpointing.md).

## Distant lights

`LightSource "distant"` accepts `L`, `scale`, `illuminance`, `point3 from`
and `point3 to`. Defaults are D65, scale 1, illuminance -1, from `(0,0,0)`
and to `(0,0,1)`. The direction toward the source is the normalized affine
vector image of **from minus to**; translation has no effect. Numeric, file,
named, blackbody and sRGB spectra use the existing illuminant preparation.
Positive illuminance multiplies photometrically normalized radiance; finite
nonpositive illuminance leaves it unchanged. This follows pinned PBRT's
implementation without an additional world-up cosine or pi factor.

Up to 64 distant lights are evaluated at each eligible surface or volume
vertex, separately from the combined 64 point/spot/goniometric/projection bound. Their
delta-direction contributions use unit weight, no inverse-square attenuation and no extra
sampler coordinates. Smooth delta-only BSDFs have no distant NEE term. Sources
have no camera-visible disk, and do not change environment/area-light MIS.

Visibility intentionally follows PBRT's finite proxy: the endpoint is the
original scattering position plus the source direction times twice the scene
bounding-sphere radius. Receiver offsets leave that endpoint fixed. Each
segment, including a segment respawned after a null boundary, ends at
`1 - 0.0001` of its endpoint parameter. This can give positive distant
illumination through unbounded homogeneous fog; it is not infinite-distance
extinction. Existing environment visibility remains infinite.

Bounds use the final prepared referenced vertices, including displacement.
Shape bounds are unioned inside each object before all eight aggregate corners
are transformed for a placement. Unused vertices and object definitions are
excluded; null and alpha-zero geometry contribute. Planar geometry can have a
positive radius. Empty or zero-radius bounds, unrepresentable bounds/diameters,
and direction arithmetic that cannot retain its represented inputs receive
source-located diagnostics. Geometry-free distant scenes are unsupported even
when emission is black. The direct scene API requires callers to provide a
positive finite `distant_scene_radius` using the same bounds convention.

The numerical profile uses world coordinates. Pinned comparisons explicitly
select PBRT's `--render-coord-sys world`, rather than claiming bitwise agreement
with its default camera-relative arithmetic. Unsupported precision at a runtime
endpoint or boundary rejects the whole pending batch. See the
[distant validation record](distant-light-validation.md).

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

## Image maps

```pbrt
Texture "paint" "spectrum" "imagemap" "string filename" "paint.png"
Material "diffuse" "texture reflectance" "paint"
```

Inline and named diffuse materials may reference imagemaps on triangles and
bilinear quads, including object instances. Supported settings and defaults
follow the [pinned PBRT texture implementation](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/textures.cpp).

| Parameter | Supported values |
| --- | --- |
| `filename` | Required entry-relative or absolute path to a supported [PNG/JPEG/EXR/HDR input](pbrt-profile.md#image-map-definitions) |
| `mapping` | `uv` (default), `planar`, `spherical`, or `cylindrical`; UV scales default to 1 and offsets to 0 |
| `encoding` | PNG: `sRGB` (default), `linear`, or `gamma` plus positive finite exponent; JPEG: fixed sRGB decode; EXR/HDR: linear regardless of valid authored encoding |
| `filter` | `point`, `bilinear` (default), `trilinear`, or elliptical `ewa`/`EWA` |
| `wrap` | `repeat` (default), `clamp`, or `black`, including non-power-of-two sources |
| `scale`, `invert` | Multiply filtered RGB by `scale` (default 1), then subtract from one when `invert` is true (default false) |
| `maxanisotropy` | EWA accepts 1–16, default 8; finite values remain inert with other filters |

Texture lookup maps the hit UV, flips the vertical coordinate and filters in
linear RGB. The spectrum uses the existing sRGB albedo reconstruction after
clamping the scaled or inverted RGB to [0,1]. Explicit texture encoding controls
sample decoding for PNG. EXR/HDR samples are always linear. PNG metadata gamma,
color metadata, and alpha do not affect reflectance.
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
EXR HALF follows the same binary16 mip path with exact decoded HALF values.
EXR FLOAT and Radiance RGBE retain binary32 mips and native views; values above
65,504 are never narrowed to HALF. Signed source values remain available to
scalar/bump consumers, subject to PBRT's nonnegative floor after NPOT resize;
spectrum reflectance clamps after filtering and
texture scale/invert. Native normal and light views retain original dimensions.
Nonfinite or unrepresentable generated mip samples fail visibly.
This texture storage policy is separate from film accumulation and output
`savefp16`. See [16-bit validation](image16-texture-validation.md).

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
PNG/JPEG/EXR/HDR with repeat bilinear sampling and takes precedence over bump mapping.
Triangle `vector3 S` tangents and normals form the declaration-space shading
frame before object placement. Surface detail changes the shading normal;
geometry, offsets, area measures, and emission sidedness keep geometric normals.
PNG normal maps decode as linear; pinned JPEG fallback decoding is always sRGB.
EXR/HDR normal maps remain linear and retain their floating source precision.

PLY shape `displacement` is true geometry displacement and requires vertex UVs.
Cyclic quads are triangulated, longest edges are refined in the shape-definition
metric with shared midpoints, displacement is evaluated in original object space,
and normals are rebuilt before Metal acceleration construction. Placement does
not trigger another refinement. Bounds are 64 refinement levels, 2²⁴ texture
node visits, the existing 2,000,000 scalar-value budget, and bounded scratch
storage. Other shape displacement remains unsupported.

Infinite image lights require a square RGB PNG/JPEG/EXR/HDR in PBRT's equal-area
octahedral projection and an orthonormal linear transform. Radiance uses native
nearest sampling with octahedral seams and calibrated RGBIlluminant/D65 spectra.
The proposal is 15/16 image-weighted sampling plus 1/16 uniform sphere sampling;
its PDF uses actual quantized CDF intervals and includes the uniform component.
The importance grid has at most 128 cells per axis; native texel mass is distributed
by area overlap into those cells, independently of native radiance resolution.
Black maps use a uniform CDF. BSDF escape and NEE use complementary power weights
with this PDF. Portals, multiple environments, instanced
emitters, and authored bilinear emission-sampling controls remain deferred.

The [core texture validation record](core-texture-validation.md) distinguishes
pinned behavior, numerical adaptations, and measured evidence. APNG, PFM, BMP, TGA, QOI, CMYK/YCCK JPEG, ICC color management, wider color
spaces and Ptex remain outside the image-input boundary. EXR/RGBE support and
linear EXR output are defined separately in the [HDR record](hdr-image-validation.md);
checkpointing and UI changes remain deferred.

## Subdivision and inline bilinear geometry

`Shape "loopsubdiv"` refines its authored triangle control mesh and projects the
result to PBRT's limit surface, including at level zero. It supplies raw limit
normals and the existing triangle default UV convention. Material and scalar
texture evaluation, alpha, medium interfaces and direct area emission use the
same paths as explicit triangles. Normals preserve relative magnitudes and signs;
transform/orientation validation applies after subdivision. This is PBRT's finite
triangle representation of the limit surface, not exact smooth-surface tracing.

`Shape "bilinearmesh"` retains exact tensor-product patches, including curved
patches, with optional vertex normals and UVs. It shares the PLY quad intersection,
texture footprint, derivative certification, alpha and light-PDF contracts below.
Its indices are already tensor ordered and do not undergo PLY's cyclic conversion.
Both families retain existing static-instance and emitting-object restrictions.

Positive area power normalizes every generated triangle or patch independently.
Increasing Loop levels increases the number of emitters and thus aggregate
authored power; it does not keep total mesh power fixed. Constant and image
emission share the existing power preparation after placement transforms.
Schema/topology limits and deferred controls are in the
[structural profile](pbrt-profile.md#subdivision-and-inline-bilinear-meshes);
selected upstream geometry and numerical evidence are in the
[validation record](mesh-shape-validation.md). No new sampler dimensions or
checkpoint schema are introduced: identity includes the complete lowered mesh.

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

For accepted triangle and patch emitter hits, an exactly zero endpoint cosine
gives the light proposal zero PDF and the BSDF technique full MIS weight,
following pinned PBRT's tangent convention. Nonzero-cosine arithmetic retains
its checked domain. This handles a finite-precision endpoint case; it does not
guarantee visibility accuracy at arbitrarily close surface junctions. See
[tangent emission validation](tangent-emission-validation.md).

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

The `dielectric-v1` extension leaves all these assignments unchanged and uses
Sobol dimension `908+b` for the dielectric lobe choice at bounce `b`. Scenes
containing any translated dielectric material, including an unused definition,
have a Sobol depth limit of 113: legacy dimensions end at 907 and the new tail
ends at 1020. Independent sampling instead takes the first Philox word of block
`0x80000000 | b`, retaining its 1024-depth limit. Smooth, rough and index-matched
interfaces use the same per-bounce allocation. CLI and EXR sampler provenance
identify this extension; scenes without dielectrics retain their original labels.

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

## Halton and ZSobol sampling

`halton` defaults to 16 samples and `permutedigits`, supporting `none`,
`permutedigits`, and `owen`; `fastowen` is invalid. Pixel samples use the pinned
base-2/base-3 mapping with resolution capped at 128 per axis. Camera values
ignore the seed; pinned Halton Owen also ignores it. Repeated seeds therefore
cannot supply independent error bars for `none` or `owen`. Checked wide index
multiplication avoids the pinned CPU implementation's signed-int overflow at
large indices; that extension is tested mathematically rather than against
undefined upstream arithmetic.

Halton retains floating variates directly. Dimensions 0/1 are camera, 2 is
wavelength, and 3 is reserved. Eight transport dimensions begin at `4+8b`.
The lens uses 998/999. Maximum depth is 124, or 110 with dielectric lobe
choices at `884+b`; dimensions never wrap. Discrete emitter choices and
roulette use scaled uniforms; area samples retain the declared midpoint grid.

`zsobol` defaults to 16 samples and `fastowen`, supporting all four Sobol
randomizations. Following pinned PBRT, a positive non-power-of-two authored
count uses the next lower power of two. A warning and provenance record retain
both authored and effective counts. The final effective budget is immutable
through dispatch and resume. Requests outside it fail before execution.
With `m=ceil(log2(max(width,height)))` and `k=log2(effective spp)`, the supported
domain requires `m<=26` and `2m+k<=52`.

ZSobol wavelength uses scalar dimension 0; its primary camera pair starts at
1 and shares the shuffled index and full 64-bit pair hash. Transport begins at
4, dielectric choices at 908, and the lens pair at 1022, retaining Sobol's
127/113 depth limits. Scalar and pair draws are distinct pinned operations.
Both new samplers use rypath's downstream dimension schedule and existing
separate material/media Philox streams; equal seeds do not imply equal PBRT paths.
See [sampling/default validation](sampling-defaults-validation.md).

## Pixel reconstruction

Box filtering samples a normalized rectangle around each pixel center, with independent
radii defaulting to 0.5. Triangle filtering samples the normalized separable tent
with independent radii defaulting to two.
Each radius must narrow to a positive normal binary32 value. Invalid radii and
unrepresentable camera arithmetic produce source-located errors before device
acquisition. Radii may exceed film dimensions when the camera arithmetic remains
supported for box and triangle filters; Gaussian has the table bounds below.

Samples retain their originating pixel and unit filter weight, including when
positions lie outside the image. There is no clipping, splatting, edge
renormalization, or additional radius factor. The selected sampler supplies the
two camera uniforms independently of wavelength and transport dimensions.
The finite uniform grids include zero and exclude one. Philox's sequence differs
from PBRT; Sobol's raw camera variates match the pinned sampler before filtering.
The [box-radius validation record](box-filter-validation.md) and
[triangle-filter validation record](pbrt-filter-validation.md)
describe inverse sampling, float bounds, and independent acceptance tests.
Half-pixel box rendering and the demo retain their original camera arithmetic.
The terminal reports the effective filter and both radii. No new CLI option is
needed; use `PixelFilter "triangle" "float xradius" [1.5] "float yradius" [.75]`
in the scene, or omit either radius to use its default of two.

Gaussian reconstruction uses radii 1.5 and sigma 0.5 by default. It follows
PBRT's full 2D midpoint table with `floor(32*radius)` cells per axis, then samples
its normalized piecewise-constant distribution. This is not a continuous
Gaussian inverse. The positive table gives a mathematically constant PBRT
`f[cell]/pdf` weight; rypath normalizes it to unit weight and retains raw counts,
with documented floating-point differences from PBRT's weighted normalization.

Tables require 1–4096 cells per axis and at most 1,048,576 total cells. Empty,
nonfinite or zero-mass tables and normalization values that would collapse on
Metal fail before rendering. Zero cells and rows remain valid; CDF inversion
skips their plateaus. The native session owns the compiled distribution, and
primary rays and texture differentials reuse the same sampled offsets.

## Spectral GGX conductors

Inline `Material "conductor"` and `MakeNamedMaterial`/`NamedMaterial` accept
independent optical constants `eta` and `k`. Each omitted constant defaults to
the original pinned PBRT `metal-Cu-eta` or `metal-Cu-k` knots. Inputs accept
built-in names, numeric wavelength/value pairs, checked entry-relative files,
RGB, blackbody, or spectrum texture graphs. RGB optical inputs use unbounded
sRGB reconstruction; each graph is compiled for its consumer role. Curves retain raw nonnegative binary32 values, including
values above one; they never pass through diffuse albedo reconstruction or
clamping. Named curves extend their endpoint values; numeric and file curves are
zero outside their inclusive authored interval. Numeric curves require at least
two wavelength/value pairs under this bounded profile. The pair must remain nonzero
throughout the renderer's 360–830 nm wavelength domain. The alternative
`reflectance` input uses pinned PBRT's eta=1, k=2*sqrt(R)/sqrt(1-R) approximation
with R clamped to [0,0.9999]; supplying it together with eta or k is an error.

`roughness` defaults to zero. Each of `uroughness` and `vroughness` independently
falls back to the effective common roughness, including inherited attributes.
These values accept finite nonnegative floats or the existing scalar texture
graphs, with no upper clamp. `remaproughness` defaults to true and applies square
root. A maximum alpha below `1e-3` selects smooth reflection; otherwise each
axis is floored to `1e-4`, following pinned PBRT's declared distribution model.
Conductors retain
the existing geometry, alpha, bump/normal map, and instance semantics; even a
constant conductor prepares a surface tangent frame for anisotropic scattering.
Unused material declarations and their dependencies are checked as well.
Mapped normals and tangents form a single orthonormal frame. Normalized tangent/
normal residuals above `64 * FLT_EPSILON`, or degenerate frames, fail visibly;
smaller roundoff residuals are projected away. The geometric normal controls
emission and ray offsets, without an extra geometric-hemisphere BSDF clip.

The transport uses a single-scattering anisotropic Trowbridge–Reitz (GGX) model
with spectral complex-IOR Fresnel and a smooth reflection limit. It omits
multiple-scattering microfacet compensation. The [PBRT material reference](https://www.pbrt.org/fileformat-v4#materials)
defines the input parameters. Rough continuation uses `f * absCos / pdf`, with
matching light-sampling PDFs and MIS. Smooth events skip direct-light proposals
and subsequent emitter/environment hits receive full weight. Null visible-normal
samples and zero Fresnel preserve prior contributions and terminate continuation.
Support, cosines, PDFs and weights use the same local BSDF frame. An accepted
grazing sample remains accepted when binary32 conversion to a normalized world
ray rounds its recomputed world cosine through zero; it is not resampled or
discarded. World-ray normalization must still produce a finite direction.
Negative/nonfinite evaluated roughness and unrepresentable optical/scattering
arithmetic fail the entire staged batch; no sample or radiance clamp hides errors.
The [conductor validation record](conductor-validation.md) describes numerical
adaptations, independent BSDF tests, and matched upstream renders.
Portable scene tests cover the input and translation contract and do not establish
GPU rendering agreement or full external-corpus support.

## Spectral GGX dielectrics

Inline and named `dielectric` materials accept one scalar `float eta`, or a
named/numeric/file `spectrum eta`, defaulting to 1.5. Effective float eta takes
precedence over spectral eta regardless of declaration order, following pinned
PBRT. An ignored spectral filename is not an input dependency. Optical curves
remain raw, finite and nonnegative; they receive no diffuse albedo clamp.
Named curves extend endpoints; numeric/file curves are zero outside their
support. Exact zero evaluated eta becomes one, including those zero tails,
matching PBRT's explicit index-matched fallback. Negative, nonfinite, overflowing,
or authored nonzero-to-zero narrowed eta values are errors. RGB and blackbody
eta use continuous optical spectral graphs. Numeric/file curves require two knots.

Roughness, per-axis fallback, remapping, smooth threshold, axis floors and
scalar graphs follow the conductor profile. Existing alpha, bump/normal-map
precedence and shape/placement frame semantics apply. Frames retain authored
orientation on both sides of an interface. The local outgoing direction selects
eta or its reciprocal; eta below one is valid. Eta exactly one is delta
straight-through transmission even at nonzero roughness.

The single-scattering GGX BSDF samples a visible normal and then chooses
reflection or refraction with its Fresnel probability. Both continuous PDFs
include that choice; transmission includes the refraction Jacobian. Transport
uses radiance convention, including reciprocal squared relative eta on
transmission. Smooth sampled weights cancel their Fresnel branch probabilities.
The original fixed compensated roulette remains unchanged; no throughput-based
eta-scale roulette is introduced. The [PBRT dielectric model](https://pbr-book.org/4ed/Reflection_Models/Rough_Dielectric_BSDF)
and pinned implementation define the target equations.

Environment and finite-emitter NEE evaluate both local hemispheres with the
absolute receiver cosine and the matching mixture PDF. Delta events skip NEE
and receive unity weight at subsequent emitter/environment hits. Geometric
normals govern offsets and emitter sidedness. Valid null microfacet samples
retain earlier contributions; invalid or unrepresentable arithmetic fails the
entire pending batch. No roughness/radiance clamp, sample retry or selective
rejection hides numerical failure.

This is a surface-interface model: each material authors a relative IOR, without
an automatic nested IOR stack. Homogeneous absorption/scattering use explicit
medium interfaces as described below. Specialized caustic sampling remains unsupported. Thin and coated surfaces
are separate models described below. Other
glass boundaries still occlude direct shadow connections; BSDF paths traverse
successive interfaces. Difficult specular-chain lighting can have high variance.
See [dielectric validation](dielectric-validation.md) for independent probes,
matched upstream images and numerical limits.

## Extended surface materials

Inline and named bindings support these additional models. All retain the
existing geometry, alpha, texture mapping and selected-material bump/normal
semantics. Unsupported parameters and malformed dependencies fail with source
locations before rendering; declarations are checked even when unused.

| Material | Parameters and behavior |
| --- | --- |
| `thindielectric` | Relative `eta`, including the dielectric float precedence; Fresnel thin-sheet reflection and straight-through transmission with internal reflection summation, without refraction or thickness absorption |
| `diffusetransmission` | Spectrum `reflectance` and `transmittance` (default .25 each), float `scale` (default 1), with texture-capable reflection/transmission spectra; each product is clamped to [0,1] after scaling, with cosine sampling in either hemisphere |
| `coateddiffuse` | Base `reflectance`; interface `eta`, `roughness`/`uroughness`/`vroughness`; internal `thickness`, `albedo` and `g`; `remaproughness`, `maxdepth`, `nsamples` |
| `coatedconductor` | `interface.eta`, `interface.roughness`/`interface.uroughness`/`interface.vroughness`; `conductor.eta`/`conductor.k` or `reflectance`, and corresponding conductor roughness; internal layer controls as above |
| `mix` | Two names in `string materials`, plus float/texture `amount` (default .5); bounded, cycle-checked nested graph, resolved once at each hit before applying the chosen child's surface detail |
| `measured` | Entry-relative `string filename` for spectral tensor-v1 reflective tables; isotropic and full-azimuth anisotropic data; reduced azimuth domains and non-spectral variants explicitly rejected |
| `fourier` | Entry-relative `string bsdffile` for SCATFUN v1, one basis and one or three channels; reflection/transmission and radiance eta weighting within otherwise supported v4 scene syntax |

Coatings adapt the pinned PBRT finite stochastic layered model. The defaults are
`maxdepth=10`, `nsamples=1`, `thickness=.01`, `albedo=0`, `g=0`; accepted authored
bounds are depth 0–1024 and samples 1–64. Thickness is floored internally to the
smallest normal float as in PBRT, and HG anisotropy is limited internally to
[-.99,.99]. Conductor optical constants are relative to the coating. This is an
explicit **compatibility approximation**: finite internal truncation and
stochastic value/PDF estimates do not establish an uncapped unbiased layered
solution. Diagnostics and `rypath.materialModel` EXR metadata record each
coating's depth/sample settings. The outer path depth remains independent.

Mix selection hashes the world hit position, outgoing direction and stable
material ID; it is independent of allocation addresses and execution order.
Amounts outside [0,1] select endpoints. At most 64 nested mix nodes are allowed.
The selected child supplies its complete scattering and surface-detail behavior.

Tables are immutable, deduplicated inputs with checked relative offsets and
allocations: at most 128 MiB per file and 512 MiB aggregate working storage.
Malformed distributions, nonfinite samples, invalid shapes, overlapping fields
and unsupported versions fail visibly. Fourier zenith grids may duplicate zero
at the reflection/transmission boundary, and inactive rows may have zero mass.
Decreasing CDFs are rejected, including the archived roughglass asset discussed
in the [validation record](surface-material-validation.md).

One-channel Fourier data is achromatic. Three-channel angular RGB is evaluated
first and then reconstructed with unbounded linear sRGB spectra. There is no
albedo clamp; this is a documented spectral adaptation of RGB data, not a claim
that legacy RGB measurements acquired spectral information.

The `material-local-philox-v1` stream derives variable-length work from pixel,
absolute sample index, bounce, seed and stable material ID, preserving all outer
sampling dimensions. Layered continuation carries its joint-path throughput
separately from the approximate directional MIS density. Directional densities
use one fixed density-only random stream for both MIS techniques; transport and
value estimates retain separate event streams. Delta classification determines
whether direct-light proposals and subsequent-hit MIS apply. Complete batches
still commit raw XYZ, independently of display conversion.

See [surface material validation](surface-material-validation.md) for references,
measured evidence, numerical limits and external-asset coverage. These additions
do not imply general PBRT v3 parsing, arbitrary layer stacks, heterogeneous media,
subsurface transport, hair/curves, or full PBRT corpus compatibility.

## Homogeneous media

`Integrator "volpath"`, including the pinned default when omitted, enables
homogeneous participating media. `path` retains vacuum surface behavior and rejects
active camera media or placed medium transitions. Unused medium definitions are
still validated. The terminal and output provenance identify applied defaults.

| Parameter | Default and interpretation |
| --- | --- |
| `sigma_a`, `sigma_s` | Independently default to one; finite nonnegative absorption/scattering coefficients; RGB uses Unbounded reconstruction, not reflectance clamping or illuminant normalization |
| `scale` | One; nonnegative multiplier for both coefficients, independent of emission |
| `Le` | Zero; finite nonnegative emission spectrum with Illuminant RGB reconstruction |
| `Lescale` | One; nonnegative emission multiplier after PBRT photometric normalization |
| `g` | Zero; authored finite asymmetry, with the pinned HG evaluation/sampling clamp to [−0.99, 0.99]; effective changes are reported |
| `preset` | Empty; otherwise exact case-sensitive name from the 47 pinned measured presets |

Coefficients and emission accept numeric, built-in named, file, RGB and blackbody
spectra. RGB reconstruction requires effective sRGB color space. Each medium
owns 471 binary32 samples from 360 through 830 nm, then uses nearest-integer
wavelength lookup, including the pinned half-way rounding convention. Numeric
and file spectra are zero outside their recorded wavelength interval; built-in
names retain their separately prepared endpoint behavior. Spectral preparation,
scaling and normalization fail visibly when values exceed the finite binary32
domain. Texture coefficients, heterogeneous media and grid/cloud/NanoVDB
representations remain unsupported.

All 47 measured names and raw RGB values are pinned in
[`medium_presets.inc`](../third_party/pbrt/medium_presets.inc). A recognized preset
supplies both coefficients using sRGB Unbounded spectra, independently of the
current color space. Explicit coefficient parameters then conflict and fail;
unused inherited coefficient defaults are ignored without opening their files.
An unknown preset warns and falls back independently to explicit coefficients
or the default one. The published table records reduced scattering in mm⁻¹;
matching PBRT, those values become `sigma_s` directly. No `(1-g)` adjustment or
automatic conversion of scene units is applied. Authored `scale` controls the
coefficient scale.

Nonzero `Le` is divided by its PBRT photometric normalization and multiplied by
`Lescale`; RGB normalization uses its base illuminant. Medium emission has source
term `sigma_a * Le`, including attenuation along each segment. Consequently a
pure-scattering medium with zero absorption emits nothing under this convention.
Authored zero emission stays zero. A positive spectrum with no positive finite
photometric normalization fails, including support wholly outside the sampled
range; blackbody emission uses stable normalization before dense storage.
`scale` changes the absorption/scattering coefficients rather than directly
scaling the prepared emission spectrum.

`MediumInterface` supplies explicit inside/outside media on geometric-normal
sides, preserving winding, reflection and `ReverseOrientation`. A camera starts
in the outside medium captured at its declaration. Named definitions are global;
shape bindings follow attribute/object scopes and object instances. Equal
inside/outside names retain the incoming ray's current medium. Authors must
supply consistent boundaries; no watertightness proof or automatic overlapping
volume stack is provided. Medium declaration transforms do not rescale a
homogeneous coefficient: attenuation uses world-space segment length.

`interface` is a null material; inline `none` and the empty material name retain
PBRT's aliases. Such geometry updates the current medium and continues the ray
without a surface scattering event. Null materials cannot be mix children or
area emitters. Null boundaries and distinct-medium transitions require proven
opaque alpha. Boundary-chain work has a separate visible failure limit; it does
not silently consume scattering depth or drop paths.

Ordinary glass surfaces can carry medium interfaces, and transmission updates
the current medium. Surface `eta` stays the authored relative IOR; medium names
do not infer a nested IOR stack. Direct-light shadow rays cross null boundaries
with medium transmittance, while glass and other ordinary surfaces occlude those
rays. There is no straight-through glass-shadow approximation or specialized
caustic sampler. Finite scattering-depth and existing material-model limits
continue to apply.

Variable-length medium work uses `medium-local-philox-v1`, a separate versioned
Philox stream under every supported sampler. The selected camera and
surface sequence is retained; volumetric events form an explicitly labeled
hybrid. Samples still enter raw XYZ accumulation only through complete validated
batches. No denoising, tone mapping or radiance clamp is introduced.

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
named-spectrum lookup; it does not enable other material models or film sensors.
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

Diffuse materials use the [existing Lambertian NEE/MIS](rendering.md); conductors and dielectrics
use their scattering contracts above. All retain compensated roulette. Escape and sided surface emission precede the depth cutoff.
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
radiance. An empty image filename is inert. An RGB PNG/JPEG/EXR/HDR filename uses native
resolution, clamp-wrapped bilinear sampling at `(u, 1-v)`, and RGBIlluminant/D65
spectral reconstruction. Combining a nonempty filename with effective `L` is
invalid. Image emitters retain the existing geometric proposal and MIS density.
Positive `power` applies independently to every generated triangle or bilinear
patch, including primitives created by displacement refinement. It retains authored
`scale` and multiplies radiance by `power / (pi * area * sides * imageMean)`, where
`sides` is two for `twosided` and one otherwise. `imageMean` is one for all spectral
`L` forms, including RGB; RGB does not introduce another luminance divisor. For an
image it is the arithmetic mean of native linear RGB texel luminance, using the
existing sRGB coefficients. A positive finite image mean is required even with
zero scale or an unused declaration. Black RGB spectra remain black; numeric
spectra retain their existing positive photometric-integral requirement. Image
power folds into image scale before multiplying the reconstructed spectrum, so
small native radiance can retain a representable final result. Positive-power
constant declarations reject nonzero base emission lost during binary32 packing;
they do not silently turn that unsupported domain into black.

Preparation uses final rounded world vertices after displacement. Triangle areas
use the world-space cross product; bilinear areas follow pinned PBRT's rectangle
classification or fixed 3×3 quadrilateral estimate, accumulated in binary64. This
normalization area does not replace the local differential area used in PDFs.
The positive factor must be normal binary32, and nonzero GPU intermediates and
products must remain finite normal binary32. Predictable failures receive
source-located errors; runtime invalid arithmetic fails the whole sample batch.
Finite nonpositive power canonicalizes to zero and preserves legacy output.
Power remains binary64 until per-primitive preparation. Transformed emitters use checked world-space
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
preserving negative and above-one values. These historical PFM comparisons
precede product EXR output; the [HDR record](hdr-image-validation.md) adds
independent EXR readback and floating-input comparisons.

```sh
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir .cache/pbrt-reference/comparison
```

## Static glTF material extension

`rypath-gltf-v1` is a dedicated core metallic-roughness spectral adaptation with
direct RGB texture slots, all core sampler modes, native top-left glTF UVs,
signed tangent normal maps, geometric sidedness and coverage. It is separate
from ordinary PBRT material graphs and requires the corresponding glTF-tagged
imagemap policy. OPAQUE and MASK emission are supported; fractional BLEND emission
is rejected. Visibility/emitter coverage uses LOD zero independently of shading
footprints. The [glTF profile](gltf-import.md) lists exact fields, numeric limits,
model assumptions and current validation boundaries. Generated worlds require
rypath; no stock-PBRT image equivalence is claimed.
