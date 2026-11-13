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
| Materials | Two-sided diffuse, omitted reflectance 0.5, linear sRGB triplets in [0,1], or numeric piecewise-linear spectra with at least two increasing knots and values in [0,1] |
| Textures | Named spectrum constants and UV-mapped PNG imagemaps for diffuse reflectance; float constants are validated during translation |
| Environment | Zero or one constant infinite light with nonnegative numeric spectrum, nonnegative linear sRGB emission, or omitted L using sRGB D65; scale defaults to 1, illuminance defaults to -1 |
| Area lighting | Directly placed diffuse triangle and bilinear patch emitters; numeric/RGB/default-D65 emission, nonnegative scale (default 1), and twosided (default false); finite nonpositive power is inert, positive power unsupported |
| Acceleration | Omitted or untuned BVH descriptor maps to native Metal structures |
| Output metadata | Filename and FP16 setting explicitly overridden by PNG policy; no bitwise PBRT PNG equivalence |

Supported parameter defaults include film 1280×720, camera FOV 90, shutter [0,1],
ISO 100, seed zero, and depth five. Independent sampling defaults to four
samples per pixel. Sobol defaults to 16 samples per pixel and `fastowen`. Perspective
`focaldistance` is inert with zero lens radius; film `diagonal` is inert for this
camera. Exact full-frame crop/pixel bounds are accepted; other bounds are
unsupported. Float narrowing rejects overflow, collapsed spectral knots or
shutter intervals, and invalid packed camera coefficients.

Non-sRGB reconstruction, legacy `color` parameters, blackbody and named/file
spectra, other image and procedural textures, textured emitters, emitting object
members, other external mesh formats, tangents/normal maps, media, motion, lens
effects, and other cameras, filters, samplers, and integrators remain unsupported.
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
spectrum value may be linear sRGB or a numeric piecewise-linear spectrum with at
least two increasing wavelengths and reflectance values in [0,1]. An explicit
`rgb value` uses the `Texture` declaration's color space. An RGB value inherited
from `Attribute "texture"` uses the color space captured at that attribute.
Direct RGB reflectance continues to use the material dictionary's color space.

Translation checks every constant definition, including unused ones, before
rendering. All references to a spectrum constant share its compiled diffuse
material payload. Float constants accept signed values that narrow to finite
binary32. Alpha masking and displacement remain deferred. See the
[constant texture validation record](pbrt-texture-validation.md).

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
| `filename` | Required entry-relative or absolute path to a supported [PNG input](pbrt-profile.md#png-imagemap-definitions) |
| `mapping` | `uv` (default), with `uscale` and `vscale` defaulting to 1 and `udelta` and `vdelta` to 0 |
| `encoding` | `sRGB` (default) or `linear`; output color primaries are sRGB |
| `filter` | `bilinear` (default), selecting one mip level |
| `wrap` | `repeat` (default), `clamp`, or `black`; non-power-of-two images require repeat or clamp |
| `scale`, `invert` | Multiply filtered RGB by `scale` (default 1), then subtract from one when `invert` is true (default false) |
| `maxanisotropy` | Accepted finite value, inert with bilinear filtering |

Texture lookup maps the hit UV, flips the vertical coordinate and filters in
linear RGB. The spectrum uses the existing sRGB albedo reconstruction after
clamping the scaled or inverted RGB to [0,1]. Explicit texture encoding controls
byte decoding. PNG gamma, color metadata, and alpha do not affect reflectance.
Float imagemaps, non-UV mappings, other encodings or filters, alpha masking,
displacement, and image emission are outside this profile.

Triangle UVs default to `(0,0), (1,0), (1,1)`. Bilinear quads default to patch
parameters. The footprint uses the geometric tangent plane and the declared
sample count. Camera ray differentials supply primary footprints. Secondary
hits and primary projection fallbacks use PBRT's camera footprint approximation.
The [image texture validation record](image-texture-validation.md) describes
mip generation, the footprint calculation, and comparison coverage.

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
radiance. An empty image filename is inert; nonempty filenames remain inventoried
unsupported assets, and combining one with effective L is invalid.
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
[constant texture validation record](pbrt-texture-validation.md), and
[image texture validation record](image-texture-validation.md).

Twenty-two owned wrappers and five labeled CC0 Cornell derivatives cover
numeric-spectrum and RGB backgrounds, diffuse response, area emission, visibility,
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
