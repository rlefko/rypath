# PBRT Sobol validation

This slice adds PBRT `sobol` with `none`, `permutedigits`, `fastowen`, and
`owen` randomization. It changes sample placement and correlation, retaining
the existing physical model, transport PDFs/MIS weights, compensated roulette,
finite authored depth, complete batches, and raw XYZ accumulation. The demo
and PBRT `independent` sampling retain their Philox sequence.

## Contract and provenance

The [rendering profile](pbrt-rendering.md#sobol-sampling) specifies the stable
dimension map, signed seeds, defaults, depth 127 bound, and 52-bit interval
index domain. Direction numbers, interval matrices, hashing, randomizers,
and binary32 conversion follow [pinned PBRT b0bdf461](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
The [source record](../third_party/pbrt/sobol.md) retains both source licenses,
checksums, and table layout. Builds need no network or PBRT runtime dependency.

Each GPU lane computes its interval index once from the full film and absolute
pixel/sample identity. A 234,208-byte owned read-only Metal buffer contains
the tables; C++/MSL layout assertions guard its representation. Upload and
command completion follow the existing synchronous resource lifetime contract.
Sobol never wraps dimensions or falls back to independent sampling.

Camera dimensions remain unscrambled, as in PBRT. Seed replicates randomize
transport conditional on that fixed spatial sequence. Filtered raster positions
can differ through arithmetic rounding, and large global pixel coordinates lose
subpixel precision in PBRT's binary32 remapping. The safe matrix bound is not a
precision guarantee. rypath uses its own fixed transport dimension schedule;
matching seeds do not promise matching paths or pixels.

## Acceptance fixed before measurement

Portable checks compare eight pinned upstream known-answer cases covering all
four modes, signed seed extremes, hashes, indices through 52 bits, camera
remapping/clamps, all interval scales, and power-of-two stratification. The
oracle was linked against the existing pinned float PBRT reference build.
All 55,900 direction and padded interval values were independently compared
with the upstream source. The table include is also checksum-checked by CMake.

The native diagnostic kernel compares 1,921 CPU/MSL queries, including every
randomization mode, dimensions through 1023, asymmetric pixels, large sample
indices, binary32 variates, and padded output lanes. Production tests separately
check camera/wavelength words across 32-bit carry and the last valid index,
direct-request rejection, odd/rectangular films, pixel order, dispatch widths,
and exact raw film equality across batch sizes 1 and 17.

A unit-radiance sky tests spectral integration for every mode with 65,536
samples. The nearest-nanometer CIE observer has half-width endpoint plateaus;
its exact integral is the trapezoidal sum of its knots. With total variation V,
one point per 1/N interval bounds XYZ error by `V * (470/N + 2^-13) + 1e-9`.
The second term bounds displacement of the step boundaries by binary32
wavelength conversion. This deterministic bound does not use seed variance.

For each randomized mode, a unit-reflectance diffuse furnace uses sixteen
fixed seeds, each with 8,192 paths. Its replicate mean must agree with unit
radiance within six replicate standard errors plus `2e-5`. Error bars are
computed across complete replicates, never from correlated points within one
Sobol sequence. These checks are evidence, not a proof of unbiasedness.

Finite-light tests distinguish three live dyadic slots from a null fourth
slot and exercise the one-emitter shift boundary. A deterministic two-plane
test checks the first 256 unrandomized paths against the assigned roulette
dimension at every bounce. Its geometry bounds prevent escape before roulette
or depth 127. Samples 0 and 1 reach that depth; 64 paths terminate at their
third scattering. Reusing the first dimension changes most expected outcomes.

The three randomized Cornell derivatives retain the existing comparison
protocol: 8×8, 4,096 spp, sixteen seeds per renderer, full-frame/quadrant linear
RGB, and `5 * combined_SE + 2e-5 * max(1, abs(reference))`. Original comparison
fixtures and thresholds are unchanged. The [manifest](../tests/fixtures/pbrt-render/README.md)
records every derivative and override. `none` uses deterministic checks above,
not repeated seeds or confidence intervals.

## Measured evidence

Hardware: Apple M4 Max, 40 GPU cores, `applegpu_g16s`, macOS 26.6 (25G5065a),
Xcode 26.6 (17F113), CMake 4.0.3, Ninja 1.13.2. Deployment remains macOS 14 /
MSL 3.1. The pinned CPU PBRT float/Release binary SHA-256 remains
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.

| Check | Result |
| --- | --- |
| M4 Max Debug, Release, ASan/UBSan | 57/57 CTests each, no skips |
| Metal API and shader validation | 10/10 native CTests, no skips |
| Metal-disabled ASan/UBSan | 49/49 CTests |
| Clang-Tidy / Apple static analyzer | 30 native C++ units, four portable stubs, and six Objective-C++ sources passed |
| Python policy/reference checks | 15/15 tests passed |
| Pinned CPU PBRT comparisons | 222/222 regional/channel gates, 576 PFM outputs |
| Unmodified Cornell product render | 1024×1024, 64 spp, all 67,108,864 samples committed |

The final probe padding/high-index additions were rerun in all three builds
and API/shader validation mode. Independent review checked table provenance,
range bounds, seed semantics, transport integration, ownership, and tests;
it prompted the step-function bound and deterministic roulette regressions.

The largest spectral absolute error was below 0.000987, against deterministic
bounds of 0.01459–0.02601. Randomized furnace means differed from one by less
than `2e-6`; bounds were at least `2.12e-5`. The largest new Cornell error/bound
ratio was 0.489 (permutedigits, top-left green); the full suite maximum remained
0.626 in the existing independent RGB multibounce fixture.

| Cornell randomization | rypath full-frame linear RGB | PBRT full-frame linear RGB |
| --- | --- | --- |
| fastowen | (0.19088398, 0.12453915, 0.034160577) | (0.19069513, 0.12442052, 0.034297471) |
| permutedigits | (0.19053561, 0.12463341, 0.034241209) | (0.19066826, 0.12447280, 0.034309642) |
| owen | (0.19067579, 0.12453113, 0.034380603) | (0.19065963, 0.12442388, 0.034300639) |

The unchanged [Cornell fixture](../tests/fixtures/pbrt/README.md) completed with
authored depth 65, fastowen seed zero, radius-one triangle filter, and default
batch size eight. Only the output filename/storage policy was overridden:

```sh
./build/release/rypath --render-pbrt tests/fixtures/pbrt/cornell-box.pbrt \
  --output build/cornell-sobol.png
```

The inspected 1024×1024 PNG has SHA-256
`db0af73215236a7439e17952dcbc4a5c7fce581330c221ac10885a4361bfdb9d`.
Its scalar spectral noise at 64 spp is visible; no denoising or raw-data
clamping was applied. This establishes one supported unmodified external entry
point, not the full PBRT scene collection or a performance claim. Raw PFM
comparisons precede display encoding. Hosted CI and CodeQL results are attached
to the PR against its final revision.
