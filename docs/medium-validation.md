# Homogeneous spectral media

This change adds a declared homogeneous-volume model to the scalar-wavelength
renderer. It changes transport and sampling, not raw Film accumulation. PBRT
semantics and the measured coefficient table are pinned to
[`b0bdf461`](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
See the [render profile](pbrt-rendering.md#homogeneous-media) for input
defaults, resource bounds and exclusions.

## Estimator

At the sampled wavelength, let `a = sigma_a`, `s = sigma_s`, and `t = a+s`.
Before sampling the next collision on a geometric segment of world length `d`,
add the analytically integrated source
`beta * Le * (a/t) * (1-exp(-t*d))`. This follows PBRT's absorption-coupled
`sigma_a * Le` emission convention. The source is zero when extinction is zero;
its infinite-distance limit is `beta * Le * a/t`. Emission is included only
while the physical scattering count is below authored `maxdepth`, matching the
pinned volumetric integrator's depth convention.

Sample an exponential extinction distance with density `t*exp(-t*x)`. If a
collision occurs before the geometric endpoint, multiply throughput by `s/t`
and sample the phase function. Otherwise reach the endpoint with unchanged
throughput: the probability of survival already supplies Beer attenuation.
The analytic source covers the whole geometric segment, independently of the
sampled flight. There is no additional collision-emission term or Beer factor
on surviving continuation paths. This is a scalar-lane adaptation of the
[volume rendering equation](https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/The_Equation_of_Transfer)
and [PBRT's volume integrator](https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/Volume_Scattering_Integrators).

Next-event estimation uses the existing separate environment and uniformly
selected finite-emitter proposals. Volume vertices use phase value without a
surface cosine. Ordered nearest-hit shadow traversal multiplies analytic
transmittance over each homogeneous interval. Complementary directional power
MIS weights exclude transmittance on both techniques; transmission is a common
integrand factor in expectation. Phase and surface continuation retain the same
preceding proposal across invisible boundaries. Volume emission has no competing
light-selection proposal and is accumulated separately.

The phase function uses PBRT's outward `wo`/`wi` convention, so
`E[dot(wo,wi)] = -g`; positive `g` favors forward propagation. Both sampling and
evaluation use PBRT's effective clamp to `[-0.99,0.99]`. The sampler uses a stable
exact inverse for nonzero `g`, including arbitrarily small `g`, and the isotropic
limit at zero. This avoids the pinned sampler's small-`g` uniform approximation.
See the [phase-function reference](https://pbr-book.org/4ed/Volume_Scattering/Phase_Functions).

## Execution and numerical contract

The versioned `medium-local-philox-v1` domain uses pixel, absolute sample index,
seed and a monotonically increasing draw-block counter. Boundary segments consume
distinct flights even when they do not consume a scattering depth. Phase/light
coordinates use separate blocks. Under Sobol this is explicitly hybrid sampling;
the preexisting camera, wavelength and surface streams remain unchanged.

Inside/outside selection uses the geometric normal and outgoing direction.
Equal-side bindings preserve the incoming medium. Invisible interfaces change
the region without consuming depth or roulette; real glass blocks a straight
shadow connection and is traversed only by sampled surface transmission.
Nested regions require consistent authored adjacent-medium names. There is no
automatic overlap priority or IOR stack.

Media share the bounded scalar GPU data buffer with auxiliary materials; the
host and shader check offsets and layouts. Empty geometry has a media-aware
sky path. Overflow, invalid arithmetic and more than 4,096 null crossings on a
flight/connection fail the entire pending batch. The existing scattering
emergency limit and authored finite-depth policy remain explicit. No failed path
is silently replaced, clamped or selectively discarded.

Small optical-depth integrals and free flights use stable series because Metal
does not provide `expm1`/`log1p`. Emission products combine binary exponents to
avoid unnecessary intermediate range loss. Zero extinction is handled before
infinite distance multiplication; positive extinction cannot escape an infinite
medium because an unrepresentable collision distance overflowed.

Existing ray-origin and endpoint offsets slightly shorten optical intervals.
Their geometric precision limits still apply; arbitrary coincident/open shells,
tiny gaps and huge coordinates are not newly guaranteed. Spectral coefficients
use PBRT's nearest-integer-nanometer dense lookup. Binary32 underflow and finite
wavelength support remain declared numerical/model limits.

## Independent checks

`metal.homogeneous_math` compares shared GPU helpers with independent long-double
equations, distribution integrals and moments. It covers 784 segment/flight
cases, 45,056 HG point cases, rotated frames, reciprocity, forward/backward signs,
near-zero and clamp-limit asymmetry, and invalid/extreme inputs.

`metal.homogeneous_transport` checks absorbing/emitting slabs, spectral
attenuation, nested/split/oriented boundaries, glass transitions and independent
single-scattering quadrature. The infinite emitting-medium oracle at depth `K`
is `Le * (1-(s/t)^K)`. Zero extinction, zero-depth emission, infinite extinction
segments, unchanged vacuum surface streams, all Sobol randomizations, batch/tile
partitioning and failed-batch accounting are covered. These are numerical
evidence for the stated model, not a proof of general scene correctness.

`pbrt.medium_scene` covers state capture, forward references, Include/object
scopes, ignored inherited preset defaults, preset conflicts/fallback, spectral
roles and files, empty scenes, null aliases and unsupported parameters.

The independently linked pinned PBRT probe verifies all 282 measured RGB
coefficients exactly, 44,274 reconstructed preset samples, 4,239 explicit
RGB/blackbody coefficient and emission samples, and 35 HG evaluations. Spectral
comparisons use the previously established reconstruction tolerances; they are
not claimed bitwise equal to PBRT. Reproduce with:

```sh
python3 scripts/compare_medium_presets.py \
  .cache/medium-validation/pbrt-probe \
  build/debug/tests/rypath_medium_scene_tests --build-probe
```

The eight [owned image fixtures](../tests/fixtures/pbrt-render/media-reference/README.md)
exercise absorption, emission, isotropic/anisotropic scattering, nested volumes,
glass, a measured preset and chromatic coefficients. Comparisons use matched
8x8 images at 4,096 spp with 16 independent seeds per renderer, retained raw PFMs,
and the existing gate of five combined standard errors plus
`2e-5 * max(1, abs(reference))` for full-image/quadrant RGB means. No thresholds
are fitted to observed results. Fixture and dependency hashes accompany the
artifacts; this is selected profile/data coverage, not full upstream scene support.

```sh
python3 scripts/compare_media.py --rypath build/release/rypath_pbrt_reference \
  --output-dir .cache/medium-image-comparisons
```

On implementation `e3f4414`, all 120 image gates pass; the largest absolute
difference is 0.498 of its predeclared threshold. All 256 raw PFM files were
independently recomputed, and matched outputs remain byte-identical after the
final mechanical static-analysis cleanup. All 47 preset comparisons pass with
maximum scaled error `6.438e-6`; the explicit RGB/blackbody comparison maximum
is `1.315e-4` under its existing per-case tolerances.

Final revision/environment and completed suite results are recorded in
[status](status.md). Large outputs remain outside Git. The owned showcase script
generates absorption, scattering, emission/nested-region and measured-preset
studios; EXRs retain linear raw output and PNGs are separate display previews.

```sh
python3 scripts/create_media_showcase.py --output-dir build/media-showcase
./build/release/rypath --render-pbrt build/media-showcase/emission.pbrt \
  --output build/media-showcase/emission.exr --batch-size 32
./build/release/rypath --render-pbrt build/media-showcase/emission.pbrt \
  --output build/media-showcase/emission.png --batch-size 32
```

The default studios use 640×400 pixels, 512 spp and depth 12. Their JSON manifest
records labels, authored coefficients/scales, source hashes and preset attribution.
These are display examples, separate from the fixed numerical comparison fixtures.
