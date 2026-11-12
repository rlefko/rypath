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
| Descriptors | Explicit perspective camera, RGB film, box filter, independent sampler, and path integrator; no substitution of omitted PBRT defaults |
| Camera | Pinhole, local +Z, PBRT raster orientation and smaller-axis FOV, aspect ratio/screen window; translation/rotation/reflection |
| Pose | Gram matrix and absolute determinant within `64 * FLT_EPSILON` of orthonormality; retain accepted basis without repair; scale/shear unsupported |
| Film | Full frame, CIE 1931 sensor, sRGB, positive ISO/shutter duration, white balance zero, no finite contribution clamp |
| Filter/sampler | Box radii 0.5; independent samples with Philox4x32-10, not PBRT's random sequence |
| Integrator | Finite maxdepth 0–1024, default 5; regularization false; omitted or bvh light selection accepted, with rypath's explicitly different sampling distribution |
| Geometry | Checked inline triangles and single-level instances, including affine geometry transforms; at least one placed triangle |
| Materials | Two-sided diffuse, omitted reflectance 0.5, linear sRGB triplets in [0,1], or numeric piecewise-linear spectra with at least two increasing knots and values in [0,1] |
| Environment | Zero or one constant infinite light with nonnegative numeric spectrum, nonnegative linear sRGB emission, or omitted L using sRGB D65; scale defaults to 1, illuminance defaults to -1 |
| Area lighting | Directly placed diffuse triangle emitters; numeric/RGB/default-D65 emission, nonnegative scale (default 1), and twosided (default false); finite nonpositive power is inert, positive power unsupported |
| Acceleration | Omitted or untuned BVH descriptor maps to native Metal structures |
| Output metadata | Filename and FP16 setting explicitly overridden by PNG policy; no bitwise PBRT PNG equivalence |

Supported parameter defaults include film 1280×720, camera FOV 90, shutter [0,1],
ISO 100, four samples per pixel, seed zero, and depth five. Perspective
`focaldistance` is inert with zero lens radius; film `diagonal` is inert for this
camera. Exact full-frame crop/pixel bounds are accepted; other bounds are
unsupported. Float narrowing rejects overflow, collapsed spectral knots or
shutter intervals, and invalid packed camera coefficients.

Non-sRGB reconstruction, legacy `color` parameters, blackbody and named/file spectra,
textured emitters, emitting object members, textures/external meshes, shading normals, media, motion, lens
effects, and other cameras/filters/integrators remain unsupported. There is no
CPU fallback. The unchanged CC0 Cornell fixture remains a negative case; this
profile does not establish unmodified Cornell or general upstream-corpus rendering.
The original Cornell still requires Sobol sampling, a triangle filter, normals,
and UV arrays. A separately named, attributed adaptation exercises supported
rendering; its [manifest](../tests/fixtures/pbrt-render/README.md) lists every change.

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
environment sample. Real triangles occupy N of M=bit_ceil(N) equally likely
slots; unused slots contribute zero. The area sample and BSDF-hit emission use
complementary power weights including the exact 1/M selection probability.
This uniform-area distribution differs from PBRT's BVH light selection and
spherical-triangle sampling. Finite visibility rays offset both endpoints and
retain blockers before the light. Numerical limits, PDFs, and fixed validation
criteria are recorded in [area-light validation](pbrt-area-validation.md).

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
[spectral validation record](pbrt-render-validation.md) and
[sRGB validation record](pbrt-rgb-validation.md), and
[area-light validation record](pbrt-area-validation.md).

Nine owned fixtures and one labeled CC0 Cornell adaptation cover numeric-spectrum
and RGB backgrounds, diffuse response, area emission, visibility, and asymmetric
multiple scattering. Comparison uses 8×8 pixels, 4,096 samples per pixel, and
16 fixed seeds per renderer (rypath 0–15; PBRT 16–31). Each channel/declared
region mean must agree within five combined standard errors plus
`2e-5 * max(1, abs(reference))`. Thresholds and seeds precede results; failures
are investigated without retries chosen to pass.

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
