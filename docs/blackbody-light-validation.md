# PBRT blackbody light validation

## Model and numerical contract

`"blackbody L" [3000]` supplies a temperature in kelvin to supported constant
infinite lights and directly placed diffuse triangle or bilinear-patch emitters.
Blackbody reflectance, named/file spectra, textured emission, emitting object
members, and positive area-light power remain outside the render profile.

The model follows pinned PBRT
[BlackbodySpectrum](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/spectrum.h),
[SpectrumToPhotometric](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/spectrum.cpp),
and [light calibration](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp).
Temperature narrows to positive finite binary32. The Planck constants are
`h=6.62606957e-34`, `c=299792458`, and `k=1.3806488e-23`.
PBRT's Wien-peak normalization cancels when dividing by the spectrum's CIE-Y sum.

rypath instead divides Planck radiance by its value at `R=830 nm`. With
`C2=h*c/k` expressed in nm K, the relative value at wavelength `l` is

```text
(R/l)^5 * exp(-C2*(R-l)/(R*l*T))
        * [-expm1(-C2/(R*T))] / [-expm1(-C2/(l*T))]
```

All 471 samples, the CIE-Y sum, and the scale/illuminance calculation remain in
double until the final checked float conversion. Exponents are nonpositive;
the 830 nm value is one, ensuring a positive normalization. This prevents
intermediate overflow and preserves faint tails that become representable after
HDR scaling. Positive temperatures rounding to zero, temperatures exceeding
binary32 range, and final emission overflow are source-located errors before
device acquisition, including invalid unused area declarations.

This is an algebraically equivalent numerical adaptation, with broader stable
temperature behavior than upstream's binary32 `FastExp` implementation.
Moderate-temperature comparisons establish agreement with the pinned renderer;
extreme cases use independent limits. RGB and numeric emission paths, spectral
storage, wavelength sampling, NEE/MIS, roulette, and raw accumulation are unchanged.

The full discrete CIE-Y sum calibrates the table. The continuous [360,830]
wavelength interval gives the endpoint lookup bins half width. In the cold
single-bin limit, discrete Y is the calibrated amplitude while expected raw
film Y is half that amplitude. This follows the existing dense spectral model.

## Independent checks

- Portable tests compare all wavelengths against direct absolute Planck
  evaluation at 1000, 2850, 6500, and 15000 K, with a `6e-8` relative packing
  tolerance. Positive binary32 temperature limits approach the 830 nm-only and
  wavelength-to-the-minus-four limits. Separate checks cover inherited/explicit
  values, scopes, ColorSpace independence, sidedness, scale, illuminance,
  diagnostics, scalar/table budgets, and retained unsupported reflectance.
- At 0.2 K and scale `1e32`, the 829 nm radiance is about `8.73899847e-8`;
  packing the relative weight before normalization would erase it. Scale `2e32`
  must fail on final radiance overflow, including an unused area declaration.
- Native per-path radiance checks use independently evaluated Planck spectra.
  The maximum error is `3.70653e-9` for the background and `1.83193e-9` for
  triangle and bilinear-patch emission, within `1e-8 + 2e-6*abs(expected)`.
  Endpoint-aware quadrature checks background, diffuse response, and emission
  with 32,768 paths and the existing six-standard-error analytic tolerance.
  The cold limiting payload gives observed Y `0.444641`, expected `0.5`, and
  analytic standard error `0.0846404`.
- The mixed blackbody scene preserves exactly equal raw films and sample counts
  across batches of 65, 7, and 1 at 5×7 pixels and 65 spp. Independent sky-off
  and area-off controls each change the image; disabling both produces exact black.

## Pinned PBRT comparison

Four owned fixtures cover warm background illumination, visible emitters with
different temperatures and sidedness, mixed environment/area multibounce
lighting, and blackbody illumination of the unmodified pinned Sportscar PLY quad.
The [fixture manifest](../tests/fixtures/pbrt-render/README.md) and
[asset attribution](../tests/fixtures/pbrt-render/ply-assets/README.md) record
settings, hashes, dependencies, and the external asset's CC BY 2.0 license.
This is selected asset coverage, not support for the original Sportscar scene.

The existing 8×8, 4,096 spp, 16-seed-per-backend protocol is unchanged: rypath
seeds 0–15, PBRT seeds 16–31, linear binary32 PFM, and
`abs(delta) <= 5*combined_SE + 2e-5*max(1,abs(reference))`.
All **402 statistical gates and two filter-sensitivity gates passed**, across
32 fixtures and 1,026 PFM outputs. The 48 new blackbody gates passed with maximum
absolute RGB mean differences of 0.002583 (background), 0.01320 (area),
0.004419 (mixed), and 0.003760 (external PLY). No seeds or thresholds were changed.

```sh
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir build/blackbody-validation/reference
```

## Measured validation

Measured November 14, 2026, for implementation `6a6d88d` based on main `acdfc160`,
with the validation tests in this PR. Host: Apple M4 Max (`applegpu_g16s`), macOS
26.6 (25G5065a), Xcode 26.6 (17F113), CMake 4.0.3, Ninja 1.13.2.

- Debug, Release, and ASan/UBSan: **70/70 CTests each**, with Metal enabled and
  both availability allowances disabled. Metal-disabled ASan/UBSan: **58/58**.
- Metal API and shader validation: **14/14** native tests, no skips. The final
  test-helper argument-order cleanup was rebuilt and passed the affected native
  test in all three presets and under API/shader validation.
- LLVM 18.1.8 clang-tidy passed the translator and both changed test units.
  Matching LLVM libc++ headers were used for the macOS SDK, as documented in
  [contributing](contributing.md). All **27 Python tests**, repository hooks,
  and independent implementation/evidence review passed.
- The product CLI rendered `blackbody-area.pbrt` with batches of seven,
  committed all **262,144 samples**, and produced an inspected PNG.

The Release reference executable SHA-256 is
`ccb1ed27b76a83f7237178c9bc0ca737ae6a2dd765803dae04640c16d34da068`;
its unchanged Metal library is
`d133c9ac81776189bfb986c689949aeb860b86758e7e821093a473ddc5a45bd4`.
The clean pinned PBRT source is `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`,
and its executable SHA-256 is
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.
The [reference build record](pbrt-render-validation.md#pinned-upstream-reference)
retains its dependency and toolchain provenance.

Hosted CI is recorded separately on the PR. No performance or broader corpus
claim is made by these checks.
