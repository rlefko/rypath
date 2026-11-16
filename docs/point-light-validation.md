# Spectral point-light validation

Implementation: `ed5c120`. The numerical measurements below cover that code;
the subsequent validation-documentation commit does not alter rendering behavior.

This change adds PBRT point-light direct illumination to the existing surface
and homogeneous-volume estimators. The [profile](pbrt-rendering.md#point-lights)
defines the 64-light bound, spectral preparation, numerical domain and deferred
light types. Raw accumulation, random streams and continuous-light PDFs retain
their existing contracts; renderer and scene-identity versions advance to 3.

## Model and references

The reference is [PBRT v4 b0bdf461](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp)
and its [delta-position sampling contract](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.h).
Intensity uses the existing photometric normalization, with RGB normalized by
its base illuminant. Positive power multiplies by `power/(4*pi)`; transforms
act on `from`. The implementation sums every point light instead of sampling
PBRT's light distribution. Surface terms are `beta*f*abs(cos)*I/r²*Tr` and
volume terms are `beta*phase*I/r²*Tr`, with no continuous-light MIS heuristic.

The point endpoint has no normal offset. Receiver visibility offsets and
alpha/null/medium traversal reuse the existing model. Ordinary glass occludes
direct shadow connections. In the absence of geometry, homogeneous connections
use analytic Beer–Lambert transmission. Point sources have no visible geometry.

## Independent numerical evidence

The native test checks inverse-square falloff, receiver cosine, additive and
wavelength-varying intensity, rough conductor/dielectric evaluation, and the
absence of a point NEE term on delta-only surfaces. Deterministic positive
signals use relative tolerance `1e-4`; zero expectations are exact. Tests also
cover 64-light traversal, rejection of 65 lights, depth zero, unchanged streams
for zero intensity, endpoint blockers, alpha, null boundaries and Beer attenuation.
Alpha uses varied camera rays because the existing coverage model hashes ray
geometry. Camera Beer attenuation is tested statistically over analog free flights.

For geometry-free single scattering, a camera at the origin looks along +Z,
with a wavelength-constant intensity-2 point at (1,0,0), absorption .3,
scattering .7 and maxdepth 1.
Independent double quadrature integrates free-flight survival, HG phase,
inverse-square intensity and light-connection transmittance. Refining quadrature
must agree within `1e-9*max(1,abs(expected))` before it is used as an oracle.
Each configuration uses 32,768 independent paths, with a gate of six standard
errors plus `2e-5*max(1,abs(expected))`.

| HG g | Expected radiance | Geometry-free residual | Standard error |
| --- | ---: | ---: | ---: |
| 0 | 0.0215358 | -0.0000154522 | 0.0000806467 |
| 0.65 | 0.00512049 | 0.00000120598 | 0.0000242181 |
| -0.55 | 0.0206784 | -0.00000715425 | 0.0000436525 |

The corresponding geometry-backed kernel cases, with off-ray null geometry,
pass the same gates; shadow boundary crossing is tested separately.
Coincident active sources fail the whole batch with zero committed samples;
zero-intensity sources skip the singular connection.

Two independently compiled shader mutations establish test sensitivity. Applying
continuous MIS to point surface lighting produces 0.346831 instead of 0.381972
and fails the inverse-square gate. Removing only geometry-free point lighting
passes the surface checks but fails HG quadrature with residual -0.0215358.
Both mutations preserve the shader compatibility handshake. They are isolated
local validation artifacts, not product variants.

## Pinned PBRT comparison and regression

The [fixture manifest](../tests/fixtures/pbrt-render/point-lights/README.md)
defines eight scenes: diffuse, additive, spectral, mixed point/area/environment,
rough reflection, rough transmission, bounded volume and external quad.
Each backend traces 33,554,432 camera samples: eight 8×8 scenes, 4,096 spp and
16 independent seeds (rypath 0–15, PBRT 16–31). All 120 region/channel gates
pass `abs(delta) <= 5*combined_SE + 2e-5*max(1,abs(reference))`. The largest
error-to-bound ratio is 0.390915. No thresholds or seeds were changed after
observing results. Full-precision linear RGB precedes display encoding.

The external wrapper retains the 412-byte pinned Sportscar quad and its
CC BY 2.0 attribution. This is selected asset coverage, not stock Sportscar or
full-corpus acceptance. Dependencies and wrapper hashes are checked before
staging; acquisition and all comparison runs remain outside routine CI.

Six unchanged scenes produce byte-identical linear PFM outputs against a clean
`cf110a5` baseline: diffuse, area-mixed, normals-instances, core-texture-lights,
rough-isotropic dielectric and isotropic medium. Point-lit surface, bounded fog
and geometry-free fog runs resume to exact Film state under both independent
and Sobol sampling, including changed batch sizes. Identity tests cover point
ordering, all position components and every spectral bin. Old contract versions
are rejected by the codec and the independent decoder.

## Reproduction and environment

Hardware is Apple M4 Max, 16 CPU / 40 GPU cores, 128 GiB unified memory;
macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang 21.0.0,
CMake 4.0.3 and Ninja 1.13.2. Portable analysis uses LLVM 18.1.8.

Debug, Release and ASan/UBSan each pass 165/165 CTests. The Metal-disabled
LLVM 18 build passes 128/128; all 115 Python tests, nine changed-unit Clang-Tidy
checks, native adapter analysis and repository hooks pass. All 39 Metal tests
also pass with API validation enabled, with no GPU availability skips.

The first API-validation run hit the existing OBJ test's 60-second timeout.
Direct runs with API validation passed on both clean `cf110a5` (63.54 seconds)
and the candidate (63.38 seconds). Its timeout is now 180 seconds, matching
other substantial render suites; the full API-validation rerun passes in
233.90 seconds. Numerical thresholds remain unchanged.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
# Repeat configure/build/test with release and asan-ubsan.
MTL_DEBUG_LAYER=1 ctest --preset debug --no-tests=error --verbose -R '^metal\.'
python3 scripts/compare_point_lights.py \
  --rypath build/debug/rypath_pbrt_reference --output-dir build/point-comparison
.venv/bin/python -m unittest discover -s scripts -p 'test_*.py'
```

These checks establish the declared point-light profile. They do not establish
many-light performance, arbitrary extreme-scale accuracy, or offline MVP completion.
