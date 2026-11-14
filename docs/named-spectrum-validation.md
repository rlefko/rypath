# PBRT named-spectrum light validation

## Model and numerical contract

`"spectrum L" "name"` resolves all 88 built-ins from pinned PBRT
[`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/util/spectrum.cpp):
16 illuminants, 21 glass/metal optical spectra, and 51 camera-response spectra.
Names are case-sensitive. The same registry classifies structural dependencies
and resolves emission; unknown strings retain their filename interpretation.
The profile accepts one bare or bracketed string on supported infinite and
directly placed diffuse triangle/bilinear area lights. Named reflectance, file
spectra, other light types, and camera sensors remain outside this extension.

Extraction preserves the pinned `FromInterleaved` endpoint extension and
binary32 evaluation order. Tables with shorter wavelength support extend their
constant endpoint values to 359 and/or 831 nm. The 16 illuminants normalize their
knots before interpolation. All named curves are sampled at integer wavelengths
360–830 nm. The existing D65 data is preserved exactly. The
[PBRT data record](../third_party/pbrt/README.md) records generation, hashes,
licenses, and the independently linked upstream oracle.

Light calibration retains rypath's existing double-precision CIE-Y sum:
`D = sum(ybar(n) * S(n))`. Packed radiance is `S(n) * scale / D`, with the
additional positive `illuminance / pi` factor for infinite lights. There is no
683 factor, RGB tint, or reflectance clamp. Every built-in has finite nonnegative
samples and a positive finite D. Scale zero yields exact black; final binary32
overflow is an error, including on unused area declarations. Stable double
intermediates can represent finite results beyond PBRT's float intermediate range.

The sparse `metal-MgO-k` curve is nonzero only at integer wavelengths 360–364 nm.
Its D is about `1.65532e-11`, and default calibrated radiance at 360 nm is about
`60411.3`. Uniform wavelengths over [360,830] give the endpoint bin half width,
so expected raw Y is about `0.8816845` despite discrete calibrated Y being one.
This is the existing nearest-nanometer spectral model. No wavelength sampling,
NEE/MIS, roulette, shader payload, or raw accumulation changes are introduced.

## Validation protocol

Require all 41,448 generated binary32 samples to match a separately linked
pinned PBRT `GetNamedSpectrum` oracle exactly. Portable tests cover name and
table identity, endpoint extension, exact zero intervals, D65 equality, source
diagnostics, scoped/inherited parameters, and resource accounting. Native tests
use independent spectral controls and complete sample batches.

The pinned renderer comparison retains 8×8 pixels, 4,096 spp, 16 seeds per
backend (rypath 0–15, PBRT 16–31), and linear binary32 PFM output. Each region
and channel must satisfy `abs(delta) <= 5*combined_SE + 2e-5*max(1,abs(reference))`.
Sparse MgO emission uses deterministic table and per-path checks rather than
a narrow image-mean tolerance. The four new owned fixtures cover background,
area emission, mixed lighting, and an unchanged licensed PLY asset in an owned
wrapper. This is selected asset coverage, not full-scene or full-corpus support.

```sh
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir build/named-emission-validation/reference
```

## Measured evidence

Measured November 14, 2026, for implementation `bd57579` based on main
`c3fcc8b`, with this PR's validation tests. Host: Apple M4 Max (`applegpu_g16s`),
macOS 26.6 (25G5065a), Xcode 26.6 (17F113), CMake 4.0.3, Ninja 1.13.2.

- All **88 names and 41,448 compiled sample words matched exactly**. Independent
  review also regenerated the include byte-for-byte and built a separate pinned
  upstream probe. The [data record](../third_party/pbrt/README.md) retains the
  canonical data and output hashes and reproduction commands.
- Debug, Release, and ASan/UBSan each passed **71/71 CTests**, with both Metal
  availability allowances disabled. Metal-disabled ASan/UBSan passed **59/59**.
  The final test-only link ordering cleanup was rebuilt and checked in each
  configuration. Metal API and shader validation passed **14/14**, without skips.
- Native independent per-path maximum absolute radiance errors were `1.49884e-8`
  for F11, `2.64658e-9` for glass-F10, and `7.93869e-9` for EOS 5D blue. MgO-k
  sky, triangle, and bilinear paths each had maximum error `0.00265348`, below
  `1e-8 + 2e-6*abs(expected)` at a peak near 60,411. All five positive bins were
  exercised and every out-of-band path was exactly dark.
- Endpoint-aware quadrature with 32,768 paths passed the existing six-standard-error
  analytic criterion. MgO-k observed Y was `0.868883` for the sky and `0.814683`
  for the patch, versus `0.8816845` expected with analytic SE `0.0520322`.
  Mixed lighting preserved exact raw films and sample counts for batches of
  65, 7, and 1; independent sky-off, area-off, and all-off controls passed.
- The full pinned PBRT comparison passed **450 statistical gates and two
  filter-sensitivity gates**, across 36 fixtures and 1,154 PFM outputs. The four
  new fixtures contributed 48 gates. Maximum absolute RGB mean differences were
  `0.01293` (background), `0.01123` (area), `0.003994` (mixed), and `0.002164`
  (external PLY). No seeds or thresholds changed.
- LLVM 18.1.8 clang-tidy passed all eight changed production/test C++ units,
  using matching LLVM libc++ headers for the macOS SDK. All **33 Python tests**,
  repository hooks, formatting, and independent implementation/evidence review
  passed. The standalone upstream probe compiled separately against PBRT.
- The product CLI rendered an inspected PNG from the owned mixed-light scene,
  with explicit preview overrides of 256×256 pixels and 256 spp. Batches of
  seven committed all **16,777,216 samples**. The PNG hash is
  `2b81fae0166a9d54d966b5b37b37294e9385f0ccfd3445fbaec0099561d0a09e`.

The Release reference executable SHA-256 is
`c0a072c721a2c1d393538bafac9a8a88313def057d3361245d28f02263a4e98b`;
the unchanged Metal library is
`d133c9ac81776189bfb986c689949aeb860b86758e7e821093a473ddc5a45bd4`.
The clean pinned PBRT executable is
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.
Its source, dependency, and build provenance remain in the
[reference build record](pbrt-render-validation.md#pinned-upstream-reference).
Hosted CI is reported separately on the PR. No performance claim is made.
