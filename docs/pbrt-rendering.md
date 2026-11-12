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
| Integrator | Finite maxdepth 0–1024, default 5; regularization false; omitted or bvh light selection |
| Geometry | Checked inline triangles and single-level instances, including affine geometry transforms; at least one placed triangle |
| Materials | Two-sided diffuse, omitted reflectance 0.5 or numeric piecewise-linear spectra with at least two increasing knots and values in [0,1] |
| Lighting | No light (black illumination) or one constant infinite light with explicit nonnegative numeric spectrum; scale defaults to 1, illuminance defaults to -1 |
| Acceleration | Omitted or untuned BVH descriptor maps to native Metal structures |
| Output metadata | Filename and FP16 setting explicitly overridden by PNG policy; no bitwise PBRT PNG equivalence |

Supported parameter defaults include film 1280×720, camera FOV 90, shutter [0,1],
ISO 100, four samples per pixel, seed zero, and depth five. Perspective
`focaldistance` is inert with zero lens radius; film `diagonal` is inert for this
camera. Exact full-frame crop/pixel bounds are accepted; other bounds are
unsupported. Float narrowing rejects overflow, collapsed spectral knots or
shutter intervals, and invalid packed camera coefficients.

RGB reconstruction, blackbody and named/file spectra, default D65 light spectra,
area lights, textures/external meshes, shading normals, media, motion, lens
effects, and other cameras/filters/integrators remain unsupported. There is no
CPU fallback. The unchanged CC0 Cornell fixture remains a negative case; this
PR does not establish Cornell or general upstream-corpus rendering.

## Spectral and transport contract

Materials retain their authored knots, narrowed to float after validation.
Piecewise-linear values are zero outside the supplied interval; materials are
not projected onto the demo's eight knots or a one-nanometer grid. Storage and
expansion work remain bounded by the preflight value budget.

For an infinite light, evaluate S at integer wavelengths 360 through 830 nm,
and compute `D = sum(ybar(n) * S(n))`. Require finite positive D. The radiance
table is `S(n) * scale / D`, additionally multiplied by `illuminance / pi` when
illuminance is positive. Zero or negative illuminance applies no additional
multiplier. There is no factor of 683. Light and observer evaluation
select the nearest integer nanometer, with half-way values rounded upward,
following the pinned dense spectra.

Each path retains one uniformly sampled wavelength with density 1/470. Raw
unexposed XYZ receives `470 * L(lambda) * CIE_nearest(lambda)`, without the
demo's Y-reference division. Film projection applies
`(shutterclose - shutteropen) * ISO / 100`, then the linear sRGB matrix. Display
conversion alone applies sRGB encoding, clipping, and eight-bit quantization.
Raw compensated double XYZ and counts remain independent of display. The
existing CIE table matches the pinned PBRT values; no new observer or RGB
reconstruction data is introduced.

The [existing Lambertian NEE/MIS and compensated roulette](rendering.md) operate
on imported spectra. Escape precedes the depth cutoff. At an opaque hit, the
authored cutoff precedes another direct-light estimate or scattering, and the
emergency watchdog. Depth zero preserves visible sky and makes opaque hits
black. This targets the finite-depth integral, not infinite-depth transport.
PBRT uses different wavelength/path sampling, roulette, and environment
sampling; comparisons are statistical rather than sample-by-sample.

Watchdog and nonfinite failures reject the entire pending batch, rather than
successfully truncating or discarding samples. High reflectance and large depth
can produce high variance with fixed-probability roulette. No performance or
arbitrary-scale geometry guarantee is made. The demo retains its unit
environment, relative observer response, and compensated stopping policy.

## Reference and validation

Routine CTests remain offline and include portable translation/camera/spectral
checks, native analytic checks, CLI failure/output behavior, and existing
geometry/demo regressions. Native tests require a GPU unless hosted CI explicitly
permits an observed missing capability; execution failures never skip.

The opt-in reference is a CPU Release/float build of the pinned PBRT revision
and exact submodules under `.cache/pbrt-reference`; it is not linked into rypath.
Its generated RGB tables are reference build assets only. Record toolchain,
dependency versions, source/submodule identities, and binary hash with each
comparison. Commands and measured evidence are in the
[validation record](pbrt-render-validation.md).

Three owned fixtures cover background, diffuse response, and asymmetric
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
