# Spectral spot-light validation

Implementation: `708bb55`. The measurements cover this implementation;
subsequent validation-documentation and sample changes do not change rendering behavior.

The [spot profile](pbrt-rendering.md#spot-lights) adds directional delta-position
emission to surface and homogeneous-volume transport, including geometry-free
media. Points and spots share a 64-source bound. Raw Film accumulation, sampling
streams and continuous-light PDFs retain their existing contracts; renderer and
scene-identity versions advance to 4.

## Model and independent checks

The reference is [PBRT v4 b0bdf461](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp).
Spot intensity uses photometric spectral preparation, inverse-square world
distance and a cubic smoothstep in light-space cosine. The inverse composed
light transform acts on the outgoing direction before normalization. Positive
power uses the cone integral; this is not a world-flux correction for affine
angular distortion. The profile documents angle, packing and numerical limits.

Independent double references check cosine-ramp fractions .25, .5 and .75
(expected factors .15625, .5 and .84375), full/zero cones, inclusive hard edges,
180-degree point equivalence, affine aiming, spectral intensity, receiver cosine,
rough conductor/dielectric reflection and transmission, and additive sources.
Deterministic positive signals use relative tolerance `1e-4`; expected zeros
are exact. Tests exercise combined limits, depth zero, unchanged sampler words,
finite endpoint blockers, alpha, null boundaries, ordinary glass and Beer
attenuation through a nonzero penumbra.

Single scattering uses independent double quadrature of free-flight survival,
HG phase, inverse-square intensity, the cone profile and Beer transmission.
The oracle must stabilize to `1e-9*max(1,abs(expected))` before use. Each
configuration traces 32,768 paths and uses the predeclared gate of six standard
errors plus `2e-5*max(1,abs(expected))`.

| HG g | Expected radiance | Geometry-free residual | Standard error |
| --- | ---: | ---: | ---: |
| 0 | 0.0115409 | -0.0000306487 | 0.0000377397 |
| 0.65 | 0.00240275 | -0.00000627487 | 0.00000816342 |
| -0.55 | 0.0141721 | -0.00000609665 | 0.0000442211 |

The three geometry-backed cases also pass; their residuals differ from the
geometry-free cases by less than `2e-9`. Separate checks cover boundary crossing.
Active coincidence and unrepresentable angular arithmetic fail the batch with
zero committed Film samples. Zero spectral intensity skips connection arithmetic.

Independent review reproduced GPU subnormal flushing that could turn a feather
into full intensity or a negative cosine into an included hard boundary. Host
threshold/packing checks and checked shader products, sums and divisions now
reject these cases. Bit-based input classification preserves the distinction
between a loaded subnormal and zero. Portable tests also reject lost aiming
components, collapsed cosine intervals and transforms made singular by packing.

Four isolated mutations of the final shader compile and link but fail the
intended gates: reversed direction, linear rather than cubic falloff, continuous
MIS on a delta source, and omission of geometry-free spot illumination. The last
mutation passes prior surface checks but fails HG quadrature with residual
`-0.0115409` against bound `0.000020`. The original source, executable, metallib,
reused AIR objects and contract headers retain identical hashes during the study.

## Pinned PBRT comparison

The [fixture manifest](../tests/fixtures/pbrt-render/spot-lights/README.md) defines
eight 8×8 scenes at 4,096 spp: penumbra, spectral power, affine transform, mixed
lighting, rough reflection, rough transmission, bounded fog and an external quad.
Each backend traces 33,554,432 camera samples with 16 independent seeds
(rypath 0–15, PBRT 16–31). All 120 full-image/quadrant/channel gates pass
`abs(delta) <= 5*combined_SE + 2e-5*max(1,abs(reference))`.
The largest difference-to-bound ratio is 0.47254. No thresholds or seed sets
were adjusted after observing results.

The affine case has one explicit **PBRT-only uniform light-sampler override**.
The initial default-BVH comparison fails nine of its 15 gates. In the pinned
implementation, `SpotLight::Bounds` retains circular world-cone angles after
nonuniform scale/shear, while `SampleLi` evaluates the inverse-transformed cone.
[The BVH sampler](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lightsamplers.h)
can consequently reject valid directions even for a single light. Switching only
the reference light sampler to uniform restores support and passes all 15 gates
with the same seeds. The source fixture is unchanged; the harness records both
backend input hashes, the exact override and its reason. This comparison checks
the pinned direct-emission model, not equivalence to that BVH culling behavior.

The external wrapper preserves the existing 412-byte Sportscar quad and its
CC BY 2.0 attribution. This establishes selected asset coverage, not support for
the original Sportscar scene or the complete upstream corpus. Manifests verify
wrapper bytes and full entry-relative dependency closures before staging.

## Regression and execution evidence

Nine unchanged scenes produce byte-identical linear PFM outputs against clean
`fc41487`: diffuse, area-mixed, normals-instances, core-texture-lights,
rough-isotropic dielectric, isotropic medium, and point diffuse/mixed/volume.
Spot-lit surface, bounded fog and geometry-free fog renders resume to exact raw
Film state under independent and Sobol sampling with changed batch sizes.
Identity tests cover every matrix coefficient, spectral bin, position component,
threshold, count and order. Independent Python serialization reproduces the
updated identity/codec goldens; authenticated contract-3 checkpoints are rejected.

Hardware: Apple M4 Max, 16 CPU / 40 GPU cores, 128 GiB unified memory;
macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang 21.0.0,
CMake 4.0.3 and Ninja 1.13.2. Portable analysis uses LLVM 18.1.8.
Debug, Release and ASan/UBSan each pass 167/167 CTests; Metal-disabled LLVM 18
passes 129/129. All 40 Metal tests pass with API validation and no availability
skips. All 119 Python checks, ten changed-unit Clang-Tidy checks, native adapter
analysis and repository hooks pass.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
# Repeat configure/build/test with release and asan-ubsan.
MTL_DEBUG_LAYER=1 ctest --preset debug --no-tests=error --verbose -R '^metal\.'
python3 scripts/compare_spot_lights.py \
  --rypath build/debug/rypath_pbrt_reference --output-dir build/spot-comparison
.venv/bin/python -m unittest discover -s scripts -p 'test_*.py'
python3 scripts/generate_spot_samples.py --output-dir build/spot-samples
```

## Demonstration images and a bounded-medium follow-up

The sample generator creates an original surface scene and geometry-free crossing
beams in unbounded homogeneous haze. Both render at 768×512 and 512 spp; the fog
demonstration also completes at 4,096 spp. PNGs are display-encoded from completed
checkpoints, with linear FLOAT EXRs retained and no denoising.

```sh
python3 scripts/generate_spot_samples.py --output-dir build/spot-samples
build/release/rypath --render-pbrt build/spot-samples/chromatic-studio.pbrt \
  --output build/spot-samples/chromatic-studio.png
python3 scripts/generate_spot_samples.py --samples 4096 --output-dir build/spot-fog
build/release/rypath --render-pbrt build/spot-fog/crossed-fog-beams.pbrt \
  --output build/spot-fog/crossed-fog-beams.png
```

The earlier generator at `7177cf8` enclosed haze in a triangle box. Its default
768×512, 512-spp Sobol render (seed 17, batch size eight) fails after 40 committed
spp with invalid arithmetic at lane 4141. Replacing its spots with points
reproduces the same failure on both this implementation and clean main `fc41487`;
replacing the sphere's coating with diffuse also leaves the failure unchanged.
The failing batch contributes no Film samples. Boundary winding and interface
scoping are valid; the exact inherited transport failure remains to be isolated.
The current demonstration explicitly changes the scene to unbounded haze with
no geometry. This is not a fix for the bounded-medium failure, which remains a
separate numerical-reliability follow-up.

These checks establish the declared spot profile, not many-light performance,
arbitrary-scale accuracy or offline MVP completion.
