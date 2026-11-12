# First progressive spectral image

`--render-demo` renders an original built-in matte scene through Metal. It is a
small declared transport model, not PBRT scene compatibility or a complete
offline renderer. There is no scene-file parser, texture system, finite light,
specular material, participating medium, denoiser, or CPU rendering fallback.

```sh
cmake --preset release
cmake --build --preset release
./build/release/rypath --render-demo --output build/demo.png \
  --width 256 --height 192 --samples 128 --batch-size 8 --seed 1
```

The output path is required. The other values shown are defaults. Progress is
reported after complete batches; only the final PNG is saved. Exit status is
`0` after rendering and publication, `2` for an unbuilt/unavailable Metal backend
or unavailable compute ray tracing, and `1` for invalid arguments or execution,
validation, or output failure. Diagnostic modes cannot be combined with rendering.

## Model and estimator

The camera is a pinhole with a jittered box pixel filter. Geometry consists of
opaque, two-sided triangles with affine instances. The geometric normal is the
normalized cross product of transformed edges, preserving reflection/winding,
then oriented against the incident ray. There is no separate shading normal.

Materials are original piecewise-linear spectral reflectances, bounded between
zero and 0.75 in the demo. Their knots are 360, 420, 470, 520, 570, 620, 700, and
830 nm. They are model spectra, not measured materials or RGB reconstructions.
The only emitter is a directionally constant equal-energy environment with
relative spectral radiance `E(lambda) = 1`. Radiance is in relative units per nm;
the image is not an absolute photometric calibration.

Each camera path samples one wavelength uniformly from 360 through 830 nm with
ideal density `1 / 470 nm`. The wavelength remains unchanged along the path.
At a surface, Lambertian scattering is `f = rho(lambda) / pi`. Independently
sample a uniform-sphere light direction with `pL = 1 / (4*pi)` and a
cosine-hemisphere continuation with `pB = max(0, dot(n, w)) / pi`.

For an unoccluded light direction above the surface, next-event estimation adds
`beta * f * E * cos(theta) / pL * wL`, where
`wL = pL^2 / (pL^2 + pB^2)`. The BSDF continuation multiplies throughput by
`rho(lambda)`. If that ray immediately escapes, its environment contribution
uses the complementary power weight `wB`. A primary miss receives full
environment radiance. The escape weight does not scale throughput at another
surface. Both queries use the same opaque visibility and ray-origin convention.

Evaluate next-event estimation before Russian roulette. Beginning after the
third scattering, survival probability is exactly 3/4; surviving throughput is
divided by that probability. Termination retains previously accumulated light.
The MIS PDFs consistently exclude roulette on both arms, whose expectation is
preserved by survival compensation. There are no delta events or light-selection
probabilities in this one-environment model.

This follows the MIS and compensated-termination construction described by
[Veach and Guibas](https://graphics.stanford.edu/papers/combine/) and
[PBRT's path tracer](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer).
Uniform environment sampling adds work and can add variance for an unobstructed
constant environment; this slice makes no sampling-efficiency claim.

There is no raw-radiance clamp, firefly removal, sample rejection, or fixed-depth
rendering policy. Needing more than 1,024 scatterings triggers an emergency failure:
the whole pending batch is uncommitted and no final PNG is published. There is
no replacement sample or altered seed retry.

## Sensor and display

The official CIE 1931 two-degree observer data are sampled at 1 nm, linearly
interpolated, and zero outside the declared wavelength range. Let `Yref` be the
trapezoidal integral of the piecewise-linear y-bar function. A returned scalar
spectral sample contributes
`L(lambda) * (xbar, ybar, zbar)(lambda) * 470 / Yref` to raw XYZ. Thus unit
equal-energy radiance has expected relative `Y = 1`. The source table is embedded
at build time from a checksum-verified local copy; builds need no download.

The table retains its own attribution and CC BY-SA license. See
[dependencies](dependencies.md) and the
[official CIE dataset](https://cie.co.at/datatable/cie-1931-colour-matching-functions-2-degree-observer).
Neither RGB upsampling nor a fitted observer curve is used.

PNG conversion uses a copy of averaged XYZ, the standard D65 XYZ-to-linear-sRGB
matrix, unit exposure, sRGB transfer encoding, and display-only gamut clipping
and eight-bit quantization. There is no white adaptation or tone mapper. The
equal-energy illuminant need not display as neutral D65 white. ImageIO tags the
PNG as sRGB. Raw XYZ sums and counts are unchanged by conversion. OpenEXR and
checkpoint/resume remain roadmap work.

## Sampling, ownership, and batches

The versioned sampler is Philox4x32-10. Its key is the 64-bit seed split into
two words; the counter is `(pixel, sample_low, sample_high, draw_block)`.
Block zero supplies camera x/y jitter and wavelength. Scattering number `k`,
starting at zero, uses block `1 + 2*k` for BSDF coordinates and roulette, and
block `2 + 2*k` for environment coordinates. Uniform floats use the upper 24 bits
times `2^-24`. Roulette uses an integer comparison with `0xc0000000`.
The [Random123 construction](https://www.thesalmons.org/john/random123/papers/random123sc11.pdf)
has a separately retained upstream license and known-answer checks.

One GPU lane traces a complete `(pixel, absolute sample)` path. A render session
owns its copied shading data, native geometry owner, pipeline, and Metal
resources. It selects the system-default device once and never substitutes a
device or CPU traversal. The native adapter binds the TLAS and declares every
referenced BLAS resident. CPU readback follows successful command completion.
All shader paths bound-check lanes; padded outputs must retain their sentinels.

Dispatches contain at most 65,536 paths. The host stages a film copy for an
entire requested batch, consumes bounded tiles in increasing sample order per
pixel, and checks complete sample accounting before committing. Raw storage is
Neumaier-compensated double XYZ sums and 64-bit per-pixel counts. Duplicate,
missing, out-of-order, or nonfinite results fail the batch. Previous complete
batches remain intact. The final tail batch uses the remaining sample count.

Batch partitions, threadgroup sizes, and pixel-tile ordering must preserve raw
state on the same device and binary. Compiler/device changes can alter floating
point paths; cross-platform bitwise image identity is not promised. Floating
uniforms quantize the ideal continuous estimator, and the wavelength expression
can round to the 830 nm endpoint. Sensor interpolation explicitly handles it.

## Geometric numerical limits

The renderer retains the [native geometry contract](geometry.md), including its
boundary limitations. Ray spawning reconstructs the hit from barycentrics,
propagates componentwise reconstruction/transform arithmetic error, adds a
declared native barycentric allowance, and rounds the offset point outward.
The allowance is `64 * FLT_EPSILON` per edge contribution, motivated by the
existing bounded geometry validation budget; it is not an Apple error guarantee.
An additional isotropic allowance is `gamma(7)` times the largest absolute
component of any object-space triangle edge, propagated through the absolute
transform. This covers native traversal uncertainty that a planar triangle's
barycentric interpolation alone cannot bound along its normal. Here
`gamma(n) = n*u/(1-n*u)` with binary32 unit roundoff `u = 2^-24`. This is an
empirical Metal adaptation within the tested domain, not a published hardware
error bound. The ray arithmetic term uses absolute product and origin magnitudes
before cancellation; disagreement with the reconstructed native hit also
contributes to the offset. The arithmetic follows the error-aware approach in
[PBRT](https://pbr-book.org/4ed/Shapes/Managing_Rounding_Error), adapted to native
Metal hits. The extent-based native-traversal allowance follows the approach in
[NVIDIA's secondary-ray analysis](https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/),
whose hardware-specific constants are not transferred to Metal.

Self-hit and nearby-blocker tests establish evidence only for the bounded
fixtures and demo. Arbitrarily small gaps, enormous coordinates, ill-conditioned
transforms, and undocumented native intersection errors are not guaranteed.
Nonfinite reconstruction and unrepresentable normals fail visibly.

## Validation contract

Portable tests cover exact sampler answers, spectral interpolation/integration,
sampling distributions, MIS and roulette algebra, display conversion, input
validation, compensated accumulation, and batch corruption. The test-only CPU
reference scans world-space triangles in double precision and uses an independent
BSDF-only path estimator with disjoint seeds. It is not a production backend.

The statistical gates are chosen before observing GPU results:

- Background and unobstructed diffuse fixtures each use 32,768 paths. XYZ means
  must agree with independent quadrature within six analytic standard errors
  plus `2e-5 * max(1, abs(expected))`. The floor variance includes both MIS arms.
- Multibounce comparison uses 16 independent replicates on each backend, each
  with a 4x4 region at 128 spp. Means must agree within five combined standard
  errors plus the same numerical floor. A path must reach a fourth hit.
- Convergence uses 16 fixed independent seeds on an analytic 8x8 floor at 16 and
  64 spp. The pooled RMSE ratio must be between 0.35 and 0.70. Individual seeds
  are not required to improve monotonically and are never rerolled to pass.
- Progressive tests compare 32 samples with batches of 1, 7, and 24 exactly,
  including odd dimensions, tail batches, and changed dispatch layout. Failed
  and incomplete batches must preserve the previous film.

Native tests additionally cover resource ownership, returned sample identities,
output guards, forced emergency termination, and transformed ray spawning.
Four fixed two-plate paths have independently computed scalar expectations for
MIS, retained NEE on roulette termination, survivor compensation, and a raw
contribution above one. They guard against accidental clamping and changes to
the estimator that aggregate image comparisons could miss.
PNG tests decode the saved image and verify conversion and failed-publication
behavior. CLI checks include unavailable backends and malformed options.

Run all three build/test presets on the final revision and run native backend
tests with API validation:

```sh
MTL_DEBUG_LAYER=1 ctest --preset debug --no-tests=error --verbose -R '^metal\.'
```

Hosted CI may use the existing explicit missing-device and missing-ray-tracing
skip options. Errors after acquiring a supported device always fail. A strict
physical M4 Max pass is required separately. CodeQL covers C++; Apple static
analysis and native runtime tests cover the Objective-C++ adapters. MSL is
compiled and exercised on the GPU. Results and remaining gaps are recorded in
[status](status.md); they are numerical evidence, not a proof of unbiasedness
or a performance benchmark.
