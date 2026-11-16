# Spectral distant-light validation

## Model and estimator

The [render profile](pbrt-rendering.md#distant-lights) follows pinned PBRT's
[construction](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp)
and [directional sampling](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.h).
Each source contributes `throughput * f * abs(cosine) * L * Tr` at a surface,
or `throughput * phase * L * Tr` at a scattering vertex. Every distant source
is evaluated once, with delta weight one and no inverse-square factor. There
is no light-selection random variable or additional sampler dimension.
Existing continuous proposals and complementary MIS weights are unchanged.

Finite endpoint distance is twice the PBRT-style scene radius, computed from
the original scattering position. The endpoint stays fixed through null
crossings, with PBRT's `0.9999` cutoff applied to each leg. This explicitly
matches finite medium attenuation, including nonzero illumination in an
unbounded medium when nonempty scene geometry supplies the radius. It is a
PBRT compatibility model, not a claim of physically infinite visibility.

The bounds hierarchy includes final displaced vertices, object aggregate
boxes and their transformed corners. Checked world-coordinate arithmetic and
the existing native-hit uncertainty domain remain numerical limitations.
Direction construction must retain cancellation-sensitive affine terms or
diagnose unsupported precision, rather than silently alter directional support.
No denoising or clamping changes raw accumulation.

The shared host/shader contract is renderer 6; canonical identity schema 5
covers the ordered distant records, all spectral bins and the scene radius.
Earlier renderer/identity contracts are rejected. Format, sampling and
accumulation versions remain unchanged.

## Independent acceptance

Portable tests check inherited spectral inputs, defaults and normalization,
source direction and affine transforms, resource limits, source locations,
empty/planar bounds, unused vertices/definitions, displacement, and the distinct
object-bound versus tight-vertex radius. Identity tests mutate every direction
component, spectral bin, radius and light order.

Native tests use independent Lambertian, spectral, GGX and homogeneous
single-scattering equations. They cover depth-zero and delta-only surfaces,
no distance attenuation, additive sources, unchanged sampling, rough reflection
and transmission, alpha/glass/null visibility, oblique endpoints, blockers
on both sides of the finite cutoff, positive finite-fog illumination, and
whole-batch arithmetic failure. Checkpoint tests compare exact Film state with
uninterrupted execution under both samplers and a changed batch size.

The nine [comparison fixtures](../tests/fixtures/pbrt-render/distant-lights/README.md)
use 16 independent seeds per backend, 8×8 pixels and 4,096 spp. The fixed gate
is five combined standard errors plus `2e-5 * max(1, abs(reference))`, applied
to five regions and three channels. Reference commands explicitly select
PBRT world coordinates and retain input/binary hashes and overrides.
The external-quad case is an owned wrapper around an unchanged pinned asset;
it does not establish support for the original upstream scene.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
MTL_DEBUG_LAYER=1 ctest --preset debug --no-tests=error --verbose -R '^metal\.'
python3 scripts/compare_distant_lights.py \
  --rypath build/debug/rypath_pbrt_reference --output-dir build/distant-comparison
python3 scripts/generate_distant_samples.py --output-dir build/distant-samples
```

## Measured evidence

Implementation `0d92160` was validated on M4 Max with macOS 26.6 and Xcode
26.6. Debug, Release and ASan/UBSan pass 169/169 tests each. All 41 Metal tests
pass with API validation enabled and no availability skips. The Metal-disabled LLVM 18
build passes 130/130 tests; all 127 Python checks pass. Changed C++ units pass
Clang-Tidy, the Objective-C++ backend passes native analysis, and repository
hooks pass.

The final Release comparison passes all 135 region/channel gates across nine
fixtures: diffuse direction, spectral illuminance, affine transform, mixed
lighting, rough reflection, rough transmission, bounded volume, unbounded
volume and the external quad. Nine existing environment, point, spot,
conductor and dielectric scenes retain byte-identical linear PFM output
against fog parent `d3cf137`.

Three actual shader mutations fail the intended independent checks: reversing
the source direction fails Lambertian radiance, halving the finite radius
misses a blocker before the cutoff, and removing the `0.9999` cutoff hits a
blocker beyond it. An independent review also exposed cancellation-sensitive
affine direction cases; linked probes and portable regressions verify the
corrected results, including an exact zero component after endpoint
subtraction. The final implementation received independent review with no
remaining actionable findings.

Both generated demonstration scenes, `afternoon-studio` and `sunlit-fog`,
complete at 768×512 and 512 spp (201,326,592 samples each), with PNG, raw EXR
and checkpoint output. They use no denoiser. These are correctness samples,
not convergence or performance claims. Local commands, binary/input hashes,
comparison statistics, mutation results and baseline hashes are retained under
`build/distant-*`; generated assets are excluded from Git.
