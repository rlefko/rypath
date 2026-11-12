# PBRT spectral rendering validation

This historical record covers the spectral subset delivered in PR #6. The
[sRGB extension record](pbrt-rgb-validation.md) documents the subsequent validation.
The current [render profile](pbrt-rendering.md) includes both slices. It is numerical evidence
for selected owned scenes, not full PBRT conformance or a performance benchmark.
Hosted results and their final commit identity are recorded on the PR.

## Local checks

Hardware: Apple M4 Max, 16 CPU cores, 40 GPU cores, 128 GB unified memory.
System: macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang 21.0.0
(`clang-2100.1.1.101`), CMake 4.0.3, Ninja 1.13.2. Product deployment remains
macOS 14 / MSL 3.1. LLVM 18.1.8 supplies formatting and focused clang-tidy.

| Check | Result |
| --- | --- |
| Debug, Release, ASan/UBSan | 47/47 CTests per preset, no availability skips |
| Metal API validation | 6/6 native CTests, no skips |
| Metal-disabled ASan/UBSan | 43/43 portable/stub CTests |
| Apple static analyzer | All five Objective-C++ sources passed |
| Policy and comparison-tool fixtures | 11 Python unittest cases passed |
| Pinned PBRT comparison | All 21 region/channel gates passed |

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
# Repeat all three commands with release and asan-ubsan.
MTL_DEBUG_LAYER=1 ctest --preset debug --no-tests=error --verbose -R '^metal\.'
cmake -S . -B build/pbrt-render-portable-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DRYPATH_ENABLE_METAL=OFF \
  -DRYPATH_ENABLE_SANITIZERS=ON
cmake --build build/pbrt-render-portable-asan
ctest --test-dir build/pbrt-render-portable-asan --no-tests=error
```

Portable tests cover effective/default parameters, typed precedence, inherited
and named bindings, instances and reflection, owned lifetimes, camera projection,
cropping precedence, source diagnostics, spectral/numeric boundaries, calibrated
film, and resource limits. Camera overflow is rejected before GPU acquisition.
The GPU tests include wide finite spectral intervals whose unscaled subtraction
would overflow, output guards, source-scene destruction, zero illumination,
batch identity, finite depth, and watchdog failure without partial samples.
Depth 1024 is checked as an accepted setting; cutoff/watchdog ordering is
exercised at a reduced coincident bound, rather than claiming a stochastic
1,024-scattering trajectory was observed.

Background, diffuse, and wide-spectrum analytic tests each use 32,768 paths and
independent double quadrature with six analytic standard errors plus
`2e-5 * max(1, abs(expected))`. Largest observed absolute XYZ differences were
0.7105, 0.005473, and 0.004586 respectively, all below their declared bounds.
The narrow blue background has large spectral variance; its Z bound was 6.338.
Existing demo and native geometry regressions remain mandatory.

## Pinned upstream reference

Source is [PBRT b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f),
with all 12 recursive submodules at the source revision's exact gitlinks.
The checkout is clean. Release uses the default binary32 Float, generated
RGB-to-spectrum tables, CPU path tracing, and no CUDA compiler. This machine
resolved installed OpenEXR 3.3.2; PBRT's other configured vendored dependencies
remain at those gitlinks. Upstream notices stay in the ignored reference cache.

```sh
git clone --filter=blob:none --no-checkout https://github.com/mmp/pbrt-v4.git \
  .cache/pbrt-reference/source
git -C .cache/pbrt-reference/source checkout b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f
git -C .cache/pbrt-reference/source submodule update --init --recursive --jobs 8
cmake -S .cache/pbrt-reference/source -B .cache/pbrt-reference/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPBRT_FLOAT_AS_DOUBLE=OFF \
  -DPBRT_USE_PREGENERATED_RGB_TO_SPECTRUM_TABLES=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build .cache/pbrt-reference/build --target pbrt_exe -j 10
```

The measured local reference executable SHA-256 is
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.
Local provenance also records submodule identities and generated table hashes.
Checkout/build disk footprints were approximately 440/301 MiB, not measured
network transfer sizes. No external scene corpus was downloaded. The pinned
Cornell fixture also passed upstream `--format`; it remains unsupported for
rypath rendering because its materials and area lights exceed this profile.

## Matched rendering comparison

The [fixture manifest](../tests/fixtures/pbrt-render/README.md) records all three
owned files, hashes, geometry, dependencies, settings, and regions. Each renderer
traces 12,582,912 camera samples across the three scenes: 8×8 pixels × 4,096 spp
× 16 fixed seeds. PBRT uses its default camera-world coordinate representation;
rypath uses world coordinates. Fixtures stay in a bounded ordinary float domain.
Only seed and output path differ between repeated inputs. The shared fixtures
specify `savefp16=false`; PBRT's output override writes PFM. No physical model,
depth, sampling budget, or sensor override is used to improve agreement.

Every region/channel passed `abs(delta) <= 5 * combined_SE + numerical_floor`.
The largest error-to-bound ratio was approximately 0.430 (upper-right red in
the multibounce scene). Full-frame means were:

| Scene | rypath linear RGB | PBRT linear RGB |
| --- | --- | --- |
| Background | (0.1637078, -2.5667232, 52.681619) | (0.1739172, -2.5744474, 52.706440) |
| Diffuse | (1.2091791, 0.54806661, 0.38292410) | (1.2084830, 0.54831787, 0.38339633) |
| Multiple scattering | (0.30974943, 0.38812841, 0.33839565) | (0.30966756, 0.38816868, 0.33881343) |

These comparisons preserve negative/above-one values and precede display
encoding. Scalar wavelength sampling, MIS, and roulette differ from PBRT, so
pixel or sample identity is not an acceptance criterion. Thresholds and seeds
were declared before results and were not relaxed or retried to obtain a pass.

## Review and limits

Independent review checked pinned semantics, depth/MIS behavior, shader ABI,
resource ownership/residency, diagnostics, and bounded indexing. It prompted
camera-overflow and film-bound-precedence fixes with regressions. The CLI frees
authored/checked parser storage before allocating the render session and film.

High-depth, high-reflectance inputs can have large roulette variance; extreme
binary32 path arithmetic can fail a batch. Native ray-origin bounds remain
empirical within the geometry test domain. RGB reconstruction, area lighting,
external assets, general corpus coverage, performance, and physical display
calibration are not established. Hosted GPU availability is reported separately
from strict M4 Max passes. CodeQL covers compiled C++; native static analysis,
shader compilation, and runtime tests cover the Apple adapter and MSL.
