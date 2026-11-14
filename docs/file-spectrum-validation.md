# PBRT file-spectrum light validation

## Model and input contract

`"spectrum L" "lights/key.spd"` supplies emission to supported constant infinite
and directly placed diffuse triangle/bilinear area lights. One bracketed string
is equivalent. Exact built-in names take precedence. File reflectance, spectrum
constant-texture values, other light types, and textured emission remain deferred.

Preflight owns bounded plain-text decoding, effective-parameter selection,
dependency accounting, and shared immutable binary32 knots. Paths use the scene
entry directory without a working-directory fallback. Translation never reopens
files. Failed attempts are cached and retain their incurred resource charges.
The [structural profile](pbrt-profile.md#spectrum-files) specifies the
grammar, limits, and diagnostics.

The semantic reference is PBRT v4
[`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f),
specifically [`ReadFloatFile`](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/file.cpp)
and [`PiecewiseLinearSpectrum::Read`](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/spectrum.cpp).
rypath requires complete decimal tokens and at least two pairs, accepts a final
number without trailing whitespace, and handles a comment immediately after a
number. These deliberately avoid the pinned reader's partial-token, EOF, and
adjacent-comment behavior. Representable binary32 subnormals are accepted;
nonzero tokens rounding to zero are errors rather than platform-dependent parsing.

File curves have inclusive endpoint support and are zero outside it. No constant
endpoint extension or illuminant initialization normalization is applied.
The same sampled-spectrum helper serves inline and file emission: binary32 knots,
double interpolation, then binary32 samples at integer wavelengths 360–830 nm.
Interpolation is anchored at the nearer endpoint so a small visible contribution
survives asymmetric wide intervals, even when the opposite fraction rounds to one.
This shared helper also corrects the same cancellation in inline sampled light spectra.
It is a numerical adaptation of PBRT's binary32 interpolation, not a bitwise match.

The existing light calibration computes `D = sum(ybar(n) * S(n))` in double and
requires positive finite D, including at scale zero. Emission samples must be
nonnegative; negative input is unsupported without clamping. Packed radiance is
`S(n) * scale / D`, with an additional positive `illuminance / pi` multiplier
for infinite lights. Final binary32 overflow is an error. Ordinary rounding into
the dense table is retained, including tiny interpolated samples rounding to zero.
Validation also applies to unused area declarations.

This adds an input representation to the existing nearest-nanometer spectral
model. Wavelength probabilities, nearest-bin lookup, NEE/MIS, roulette, finite
depth, Metal payloads, and raw film accumulation are unchanged. End bins at
360/830 nm retain half width under the continuous wavelength sampler; analytic
quadrature must account for that width.

## Validation protocol

Portable tests cover decoder boundaries, numeric range, parameter inheritance,
scoping, entry-relative resolution, dependency caching and combined asset budgets.
Translation tests compare file and inline curves, exercise independent calibration
controls, and remove the source file after preflight to detect reopening.

An independently linked pinned PBRT `PiecewiseLinearSpectrum::Read` probe samples
the owned spectrum files. Compare it with the actual rypath checked-file path
before calibration at every integer wavelength. The predeclared tolerance for
practical curves is `abs(delta) <= 2e-6 * peak`, where peak is the largest absolute
authored knot value. Require exact endpoints and out-of-support zeros. Extreme
numeric inputs use separate analytic controls, since PBRT's binary32 intermediates
can overflow or underflow.

Native tests use independent per-path spectral expectations, support/endpoint
controls, complete sample accounting, and exact batch invariance. Nonzero per-path
radiance uses the existing `1e-8 + 2e-6*abs(expected)` tolerance; zero is exact.
Analytic XYZ means use six analytic standard errors plus
`2e-5*max(1,abs(expected))`. Four owned render fixtures cover environment, area,
mixed lighting, and the existing licensed
PLY asset in an owned wrapper. Spectrum inputs are project-authored; the PLY retains
its CC BY 2.0 attribution. This is selected asset coverage, not support for an
unmodified external spectrum file or the original Sportscar scene.

The pinned image comparison uses the existing 8×8, 4,096-spp, 16-seed protocol
(rypath 0–15, PBRT 16–31). Each declared region/channel must satisfy
`abs(delta) <= 5*combined_SE + 2e-5*max(1,abs(reference))`. Dependencies are hashed
and staged beneath every relocated entry directory. Seeds and thresholds are
fixed before rendering; failures require diagnosis rather than selection of a
passing run. See the [fixture manifest](../tests/fixtures/pbrt-render/README.md).

For the raw oracle, build `scripts/pbrt_file_spectrum_probe.cpp` using the
[separate PBRT probe link command](../third_party/pbrt/README.md#reproducing-and-validating-the-named-data),
substituting that source and the output executable below. Then run:

```sh
python3 scripts/compare_pbrt_file_spectra.py .cache/pbrt-reference/source \
  build/file-spectrum-validation/pbrt_file_spectrum_probe \
  build/release/tests/rypath_pbrt_file_spectrum_probe \
  tests/fixtures/pbrt-render --output-directory build/file-spectrum-validation/raw
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir build/file-spectrum-validation/reference
```

## Measured evidence

Measured November 14, 2026, for implementation `87eb388` based on main
`71b6365`, with this PR's validation additions. Host: Apple M4 Max
(`applegpu_g16s`), macOS 26.6 (25G5065a), Xcode 26.6 (17F113), Apple Clang 21,
CMake 4.0.3, and Ninja 1.13.2. Both Metal availability allowances were disabled.

- Debug, Release, and ASan/UBSan each passed **72/72 CTests**. Metal-disabled
  ASan/UBSan passed **60/60**. Metal API and shader validation passed **14/14**
  with `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`, without skips.
- The separately linked PBRT file oracle passed **1,884 raw spectral samples**,
  including **1,080 exact endpoint/zero checks**. Maximum absolute errors for
  cool, endpoint, sky, and warm curves were `5.96046448e-8`, `1.49011612e-8`,
  `1.1920929e-7`, and `1.1920929e-7`, respectively, below the preset tolerances.
- The complete pinned image comparison passed **498 statistical gates and two
  filter-sensitivity gates**, across **40 fixtures and 1,282 PFM outputs**.
  The four new fixtures contributed 48 statistical gates. Their maximum
  absolute regional RGB mean differences were `0.005986` (background),
  `0.01973` (area), `0.005946` (mixed), and `0.009233` (external PLY).
  No seeds or acceptance thresholds changed.
- Native independent per-path maximum radiance errors were `7.51337e-9`
  for the sky, `0.00187407` for the narrow endpoint triangle emitter, and
  `2.89681e-9` for the imported bilinear emitter. Every control exercised all
  471 bins and required exactly zero radiance outside support or in zero bands.
  The endpoint test measured raw Y `0.848220` versus analytic `0.827806`, with
  analytic SE `0.0525068`; endpoint-aware XYZ checks passed. Mixed lighting
  retained identical raw films/counts at batch sizes 65, 7, and 1, and passed
  independent environment-off, area-off, and all-off controls.
- Independent review found the wide-interval cancellation described above.
  Both asymmetric slope regressions now pass, including exact file/inline
  payload agreement and independent checks of all 471 normalized bins. A
  separately linked negative control using the old interpolation formula fails
  on the intended case. The reviewer verified the fix with portable tests,
  M4 Max execution, and the pinned raw oracle; no actionable findings remain.
- Focused LLVM 18.1.8 clang-tidy with matching libc++ headers, all **36 Python
  tests**, formatting, and repository hooks passed. Comparator tests reject
  truncated output, nonfinite values, excess error, and perturbed exact controls.
- The product CLI rendered an inspected PNG from the mixed-light fixture with
  explicit preview overrides of 256×256 pixels and 256 spp. Batches of seven
  committed all **16,777,216 samples**. Its SHA-256 is
  `605771a02071124277d36d19bee2eb929c69ab6c5a420d07a3a723bcfaa02d58`.

The Release reference executable SHA-256 is
`b7689a3865ce2922d65fe66dc0acefc826cf2deae758a4fb0f423e57bdaf46ec`;
the unchanged Metal library is
`d133c9ac81776189bfb986c689949aeb860b86758e7e821093a473ddc5a45bd4`.
The clean pinned PBRT executable is
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`;
the separately linked PBRT spectrum probe is
`0418215e380409d5d0de84df374fff4f24a09516ac90b4d1741025f4a3ca45a5`.
Raw probe dump hashes are
`057086f9db5e0348c2e8c422ce9266eefad37b6d5142e9a54dc5b2694a52713f` (PBRT) and
`c696c1a7e20e9939562eab7994623715c5bb288cdb7e65112ce57cc1fb515165` (rypath).
Reference source/dependency provenance is retained in the
[reference build record](pbrt-render-validation.md#pinned-upstream-reference).
Hosted checks are reported separately on the PR. No performance claim is made.
