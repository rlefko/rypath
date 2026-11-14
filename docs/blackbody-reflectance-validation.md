# PBRT blackbody reflectance validation

## Model and numerical contract

Diffuse materials and spectrum constant textures accept a temperature that
narrows to positive finite binary32 kelvin. Both routes retain the same analytic
material, evaluate at the actual path wavelength, and clamp reflectance to [0,1].
Effective-parameter selection, inheritance, named material binding, and constant
texture sharing follow the existing profile. Blackbody values are independent
of ColorSpace. Unused definitions are validated too, and each effective
temperature consumes one scalar under the existing spectral limits.

The semantic reference is PBRT v4
[`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f),
specifically its
[`BlackbodySpectrum`](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/spectrum.h),
constant spectrum texture, and
[`DiffuseMaterial::GetBxDF`](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/materials.h).
It normalizes Planck radiance at the Wien wavelength `2.8977721e6 / T` nm,
including when that peak lies outside the visible interval. No CIE-Y light
calibration or visible-interval renormalization is applied to reflectance.

Using the pinned physical constants `h=6.62606957e-34`, `c=299792458`, and
`k=1.3806488e-23`, let `C2=h*c/k*1e9` in nm K, `x=C2/(lambda*T)`, and
`x_peak=C2/2.8977721e6`. The evaluator computes

```text
log(rho) = 5*log(x/x_peak) + x_peak - x
           + log(-expm1(-x_peak)) - log(-expm1(-x))
reflectance = clamp(exp(log(rho)), 0, 1)
```

This is an algebraically equivalent numerical adaptation of the continuous
Planck model. It avoids an intermediate exponential underflow that would erase
representable cold tails. CPU evaluation uses the original checked Kelvin in
double, with zero outside [360,830] nm. The diffuse clamp is part of the scattering
model, not radiance clamping or sample rejection. Sampling, NEE/MIS, roulette,
authored depth, and raw accumulation are unchanged.

Metal packs `min(C2/T, 128*830)` in the existing tagged scalar material slot,
computed in double before narrowing. The material wire record remains 64 bytes.
For `x >= 128`, the normalized value is at most `4.17e-47`, below half the
minimum binary32 subnormal, so zero is the correctly rounded result. The cap
applies only to GPU packing. Even at maximum finite binary32 Kelvin, the packed
factor and visible `x` remain normal floats.

Metal evaluates the same log ratio in float. Below `x=1/16`, it computes
`-expm1(-x)` as `x*(1-x/2+x^2/6-x^3/24+x^4/120)`; otherwise it uses
`1-exp(-x)`. The first omitted relative term at the switch is below `1.4e-9`.
This avoids cancellation for hot spectra without adding a sampled approximation
to the spectral model.

## Acceptance criteria

Thresholds were fixed before implementation measurements. The independent
high-precision reference evaluates the absolute Planck ratio separately from
the production log rearrangement. CPU relative error must be at most `2e-12`
for normal double results. Direct and texture translation routes must agree
exactly. Cases include fractional wavelengths, visible endpoints, peak neighbors,
positive binary32 temperature extremes, and both numerical switch boundaries.
Additional binary64 subnormal checks require a positive result within
`2e-12*reference + DBL_TRUE_MIN`; this accounts for relative transcendental
rounding plus the final subnormal rounding step. Results that round to zero are
checked exactly. Reference values rounding to one use the normal relative bound.

A native probe calls the production shader helper directly. Its absolute
reflectance error must be at most `2e-5`; at reference reflectance at least
`2*FLT_MIN`, it must also be nonzero and within `1e-4` relative error.
Below that boundary, tests separately characterize the transition and exact
zero limits. The relative condition is deliberately separate from end-to-end
path tests: angular weighting can underflow an intermediate even when the
reflectance itself is normal. No relative radiance guarantee is made for tiny
tails amplified by arbitrarily large illumination.

Native white-plane checks compare complete per-path contributions against
matched white samples and independent spectral expectations. Ordinary paths
use `1e-7 + 2e-5*abs(white_radiance)`; cold `x` near 100 and hot `T=1e12`
controls additionally check nonzero contributions and relative accuracy where
their transport intermediates remain comfortably normal. Batch sizes 65, 7,
and 1 must yield exactly equal raw films and complete sample counts.

A separately linked pinned PBRT probe checks 1000, 2850, 3000, 6500, 15000,
and 50000 K at every quarter nanometer from 360 through 830. Its fixed absolute
tolerance is `2.5e-4`: PBRT uses float `FastExp`, while this implementation
evaluates the continuous Planck function with standard exponentials. Extreme
temperatures use the independent physical reference because upstream's
intermediate float arithmetic can degenerate there.

Two owned fixtures exercise direct 3000 K diffuse reflectance and an inherited
6500 K spectrum constant texture on the unmodified pinned Sportscar quad. Flat
numeric lighting isolates the reflectance. The
[fixture manifest](../tests/fixtures/pbrt-render/README.md) records hashes and
settings; [asset attribution](../tests/fixtures/pbrt-render/ply-assets/README.md)
preserves the quad's CC BY 2.0 provenance. This is selected external asset
coverage, not support for the original Sportscar scene.

The full image comparison retains 8x8 pixels, 4096 spp, 16 fixed seeds per
backend, linear binary32 PFM output, and the existing regional/channel gate:
`abs(delta) <= 5*combined_SE + 2e-5*max(1,abs(reference))`.

## Measured evidence

Measured November 14, 2026 for implementation `93bda16`, based on main
`f8b8ed7`. Host: Apple M4 Max (`applegpu_g16s`), macOS 26.6 (25G5065a),
Xcode 26.6 (17F113), Apple Clang 21, CMake 4.0.3, and Ninja 1.13.2.
Both Metal availability allowances were disabled.

- Debug, Release, and ASan/UBSan each passed **74/74 CTests**. Metal-disabled
  ASan/UBSan passed **61/61**. Metal API and shader validation passed **15/15**,
  without skips. The final lint-only transport-test refactor was rebuilt and
  rechecked in Release, ASan/UBSan, Debug native tests and Metal validation.
- Routine CPU tests passed **15,049 independent absolute Planck comparisons**.
  The separate 80-digit Decimal oracle passed **32,402 samples per translation
  route** across 17 temperatures and 1,906 wavelengths. Direct material and
  constant texture results matched exactly. Maximum normal relative error was
  `2.33732396e-13`, below `2e-12`. All **8,547 exact zero** and **255 subnormal**
  checks passed; 13 reference samples rounded to one. Release and portable
  sanitizer probe dumps matched byte for byte.
- The independently linked pinned PBRT probe passed **11,286 samples per route**
  across six temperatures, with maximum absolute error `0.00018781292`, below
  `0.00025`. Explicit and inherited 6500 K PLY wrappers also produced identical
  pinned PBRT images at 32 spp.
- The native evaluator passed **9,487 cases**, including **7,808 relative checks**
  and **1,215 exact zeros**. Maximum absolute error was `5.34649e-7`, below `2e-5`;
  maximum relative error was `1.27534e-5`, below `1e-4`. Quarter-nanometer sweeps,
  switch neighbors, cold/hot tails, endpoints and padded output guards passed.
- Four native white-plane controls covered **131,072 paths**. Maximum relative
  errors were `1.47208e-6` at 3000 K, `7.41223e-7` at 6500 K, `1.37752e-5` for
  the cold 175 K control, and `1.31478e-5` at `1e12` K. The cold control included
  281 qualifying paths with expected radiance at least `0.00154575`; the hot
  control included 12,449 with expected radiance at least `0.473824`. These
  controls would fail if intermediate underflow erased the spectral tails.
  Both fixtures preserved exactly equal raw films and complete sample counts
  across batch sizes 65, 7, and 1.
- The full pinned image comparison passed **534 statistical gates and two
  filter-sensitivity gates**, across **44 fixtures and 1,410 PFM outputs**.
  The two added fixtures contributed 18 gates. Maximum regional RGB mean
  differences were `0.0003072` for direct blackbody diffuse and `0.005764` for
  the PLY texture wrapper. Seeds and thresholds were unchanged.
- All **52 Python tests**, focused LLVM 18.1.8 clang-tidy with matching libc++
  headers, native static analysis, formatting, repository hooks, and independent
  code/evidence review passed. Negative comparator tests reject malformed,
  truncated, nonfinite, out-of-range, excessive-error, and mismatched-route data.
  Existing named-reflectance and rational-oracle dumps reproduced PR #20 results.
- The product CLI published an inspected 256x256 PNG at 256 spp from the PLY
  wrapper, with those preview overrides recorded in its staged input and the
  unchanged PLY dependency staged beside it. Batches of seven completed all
  **16,777,216 samples**.

The review corrected overly strict validation assertions near the white peak
and at the binary64 subnormal boundary, without widening the agreed normal
accuracy gates. It also strengthened exact-zero and transition checks in the
GPU probe. No implementation defect remained after independent review.
Hosted checks are reported on the PR separately. This is not a performance study.

## Reproduction

Use the matching CMake configure/build/CTest presets for `debug`, `release`, and
`asan-ubsan`. For the portable sanitizer suite, configure with
`cmake --preset asan-ubsan -B build/reflectance-portable -DRYPATH_ENABLE_METAL=OFF`,
build that directory, and run `ctest --test-dir build/reflectance-portable
--output-on-failure --no-tests=error`. For strict Metal checks:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --preset debug --no-tests=error -R '^metal\.'
python3 scripts/compare_blackbody_reflectance.py .cache/pbrt-reference/source \
  build/blackbody-reflectance-validation/pbrt_blackbody_reflectance_probe \
  build/release/tests/rypath_reflectance_curve_probe \
  --output-directory build/blackbody-reflectance-validation/release
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir build/blackbody-reflectance-validation/reference
```

Build `scripts/pbrt_blackbody_reflectance_probe.cpp` using the separate pinned
PBRT link command in the [data record](../third_party/pbrt/README.md#reproducing-and-validating-the-named-data),
substituting this source and the output path above. Source/dependency provenance
remains in the [reference build record](pbrt-render-validation.md#pinned-upstream-reference).
Extended comparisons remain opt-in; routine CTest and hosted CI need no external
reference checkout or new downloaded assets.

SHA-256 artifact identities:

| Artifact | SHA-256 |
| --- | --- |
| Release rypath reference executable | `eee3f67d2c174b7e0faf50ccb98eaf0a27614280a7f8b3552441f63fa0e7de57` |
| Release Metal library | `d9f671dbecdcbd55918716562abb5fa90228801f7922777c8f327538ddc383a5` |
| Pinned PBRT executable | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |
| Pinned blackbody probe | `287b3e114ecc707aa8a51909d0a20b7ae24f08ef375fd939ee7020113cfceb35` |
| Query dump | `649e74b39f59019bd78161df5567f8a15611b36ac1d96d9c0de9ef8b38f631fb` |
| rypath binary64 dump | `7ee61caf14b409f29ac6622cc79b828548dca8a79c3c5d761f50cdb8fa17219b` |
| PBRT binary32 dump | `11aa7bb86a64d897fd70d600cf905a80cd4ea31ad6717a86e3d6c178b688aebc` |
| Full image-comparison log | `e6566c31e9ee35ec8d98c5537e842ea602e4166dd021ea9e849aa98996f225d1` |
| CLI preview PNG | `8ebfa1a6ee33800ee2fce34c789dbbdef2cc8ba86932f39813d0c318b10228c2` |
