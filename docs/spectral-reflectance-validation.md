# PBRT spectral reflectance validation

## Model and numerical contract

Diffuse materials and spectrum constant textures accept all 88 pinned PBRT
built-in spectra and bounded plain-text spectrum files. Preflight retains shared
immutable binary32 file knots; translation does not reopen assets. Exact names
take precedence over files, paths use the entry directory, and effective values
retain existing inheritance and shadowing rules. Spectral inputs do not consume
the current RGB color space. See the [input profile](pbrt-profile.md#spectrum-files).

The reference is PBRT v4
[`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
Its [`DiffuseMaterial::GetBxDF`](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/materials.h)
clamps the evaluated spectrum to [0,1]. This is the declared scattering model,
not radiance clamping, selective sample rejection, or an estimator modification.
Signed and above-one file ordinates are therefore retained until evaluation.
Zero and out-of-visible-range curves are valid black reflectance. File support
is inclusive at its endpoints and zero outside; no photometric normalization is
applied. Existing inline numeric reflectance still requires values in [0,1].

Built-ins use 4,204 prepared original knots, including the pinned initialization
normalization and constant endpoint extension. Fractional knots must not be
replaced by integer-nanometer light data. Both generated data sets preserve their
separate [licenses and reproduction records](../third_party/pbrt/README.md).

### Compiling bounded GPU curves

For a source segment `(a, A), (b, B)` and binary32 wavelength `x`, evaluate the
numerator `b*A - a*B + x*B - x*A` using an error-free binary64 expansion. Every
product is exact because both factors are binary32, including their subnormals;
the product exponent range fits in binary64. Adding `a - b` tests the one
threshold without a rounded cancellation. The fixed six-term expansion adapts
[Shewchuk's error-free addition](https://www.cs.cmu.edu/~quake/robust.html), already
used for geometry predicates in this renderer. This curve compilation is our
adaptation, not a published spectral algorithm or a performance claim.

The compiler preserves authored wavelengths, clamps their ordinates, and adds
anchors where a segment crosses 360 or 830 nm. A crossing inside that visible
interval is located by binary search over ordered positive binary32 bit patterns,
using exact numerator signs: retain an exactly representable crossing, otherwise
retain its adjacent binary32 neighbors. At most 24 bisections per threshold are
needed. Interior ordinates use the expansion estimate divided by the segment
width, then narrow to binary32. No shader interpolation affecting a visible path
spans the original extreme wavelength interval.

Compiled knots use the existing bounded sampled-material payload. Equivalence
is checked at binary32 path wavelengths in [360,830], with `1e-6` absolute
reflectance tolerance and exact zero/one controls. Between adjacent crossing
neighbors, or outside the visible interval, this compiled payload is not a raw
spectrum evaluation API. The checked source remains separately available.
Ordinary binary32 interpolation differs numerically from pinned PBRT; extreme
cancellation cases use an independent rational oracle rather than its float
arithmetic as ground truth.

The unchanged Metal sampled-material evaluator can flush tiny positive ordinates
or interpolation products to zero, including products of minimum-normal
ordinates. This already affects inline sampled spectra. The absolute reflectance
tolerance is not a relative-error guarantee for arbitrarily small reflectance
under arbitrarily large finite illumination; such inputs can amplify otherwise
tiny spectral errors into visible radiance differences. CPU source preservation
and curve compilation do not imply subnormal transport fidelity on the device.

Source work and compiled knots are each charged at two scalar values per knot,
plus the authored string. Per-list and aggregate spectral limits apply before
allocation; failures return no partial curve. Constant texture references share
their compiled material. Existing wavelength probabilities, NEE/MIS, roulette,
finite-depth model, Metal buffers/kernels, and raw compensated accumulation are
unchanged.

Compilation first counts generated knots, then allocates and fills exactly that
many entries. The bounded crossing search runs in each pass. This avoids retained
spare capacity from geometric vector growth escaping the aggregate storage charge.

## Validation protocol

Portable CTests check source ownership, precedence, diagnostics, budget
boundaries, and translation after file removal. Analytic compiler cases cover
clamp ordering, narrow support, signed extremes, boundary-only support, invalid
knots, and complete rejection at one-less output capacity.

`compare_reflectance_curves.py` uses Python rational arithmetic independently
of the compiler. Its fixed curves include deterministic randomized binary32
inputs, actual crossing neighbors, `(-2^120,-2^111)/(2^120,2^111)` with raw value
`x/512`, and opposite maximum values. Its probe reports the actual compiled
curve, requested wavelengths, and neighbors of every generated knot. Exact
support/zero/one checks and the absolute tolerance are fixed before execution.

A separately linked pinned PBRT probe evaluates all 88 named curves at every
quarter nanometer. Compare both actual direct-material and constant-texture
translation routes within `2e-6` absolute reflectance, with exact zero/white
controls. Preserve the earlier bit-exact integer emission checks separately.

Native white-plane tests use independent per-path expectations for named gold,
a signed ramp, sub-nanometer file support, and extreme signed interpolation.
Each compares radiance with the matched white path times the independent
reflectance, within `1e-7 + 2e-5 * abs(L_white)`. Fixed paths must exercise the
intended regions. Batch sizes 65, 7, and 1 must produce identical raw films and
counts on the same device.

Two owned image fixtures cover named direct diffuse reflection and inherited
signed file values on a spectrum constant texture around the existing licensed
Sportscar quad. The [fixture manifest](../tests/fixtures/pbrt-render/README.md)
records hashes, dependencies, and wrapper settings. This is selected asset
coverage, not support for an unmodified upstream scene or external spectrum file.
The full pinned comparison retains 8x8 pixels, 4,096 spp, and 16 fixed seeds per
backend, with regional/channel acceptance
`abs(delta) <= 5*combined_SE + 2e-5*max(1,abs(reference))`.

```sh
python3 scripts/compare_reflectance_curves.py \
  build/release/tests/rypath_reflectance_curve_probe \
  --output-directory build/reflectance-validation
python3 scripts/compare_pbrt_reflectance.py .cache/pbrt-reference/source \
  build/reflectance-validation/pbrt_reflectance_probe \
  build/release/tests/rypath_reflectance_curve_probe \
  --output-directory build/reflectance-validation
python3 scripts/compare_pbrt.py \
  --rypath build/release/rypath_pbrt_reference \
  --pbrt .cache/pbrt-reference/build/pbrt \
  --fixtures tests/fixtures/pbrt-render \
  --output-dir build/reflectance-validation/reference
```

Build `scripts/pbrt_reflectance_probe.cpp` with the separate PBRT probe command
in the [data record](../third_party/pbrt/README.md#reproducing-and-validating-the-named-data),
substituting its source and the output path above. Extended comparisons remain
separate from offline routine CTest; comparator negative tests run with the
existing Python test suite.


## Measured evidence

Measured November 14, 2026, for implementation `3d437e3`, based on main
`69399568`. Host: Apple M4 Max (`applegpu_g16s`), macOS 26.6 (25G5065a),
Xcode 26.6 (17F113), Apple Clang 21, CMake 4.0.3, and Ninja 1.13.2.
Both Metal availability allowances were disabled.

- Debug, Release, and ASan/UBSan each passed **73/73 CTests**. Metal-disabled
  ASan/UBSan passed **61/61**. Metal API and shader validation passed **14/14**
  without skips. The final test-only optional guard was rebuilt and rechecked
  in all four configurations.
- The rational oracle passed **273 curves, 140,062 samples, and 109,118 exact
  zero/one controls**. Maximum absolute reflectance error was `5.7726902e-8`,
  below the fixed `1e-6` limit. Debug and Release produced identical dumps.
- All **88 built-in spectra** passed **165,528 quarter-nanometer samples per
  translation route**, with **3,549 exact zeros and 34,982 exact whites** per
  route. Maximum absolute error against pinned PBRT was `1.1920929e-7`, below
  `2e-6`. Direct material and constant texture output matched exactly. Separate
  probes matched all **4,204 original knot values** and **41,448 existing dense
  emission values** bit-for-bit; both generated includes reproduced byte-for-byte.
- Four native white-plane controls covered **131,072 paths**. Maximum per-path
  radiance errors were `1.29253e-9` (named gold), `1.01784e-9` (signed ramp),
  `5.90745e-10` (sub-nanometer support), and `1.66007e-9` (wide signed curve).
  Fixed samples exercised their declared zero/interior/saturated regions.
  Raw films and complete counts matched at batch sizes 65, 7, and 1.
- The full pinned image comparison passed **516 statistical gates and two
  filter-sensitivity gates**, across **42 fixtures and 1,346 PFM outputs**.
  The two new fixtures contributed 18 statistical gates; maximum regional RGB
  mean differences were `0.001088` for named diffuse and `0.003889` for the file
  texture wrapper. Seeds and thresholds were unchanged.
- Independent review found and verified the retained-capacity fix: a curve
  with 100 original knots and two generated anchors now retains exactly 102
  entries, and a 101-entry budget fails without a partial payload. The reviewer
  reran focused portable/native tests, both numerical oracles, and data
  regeneration; no actionable findings remain.
- Review also confirmed the inherited Metal underflow limit using an actual
  checked file with `(360, 2^-126)` and `(830, 2^-126)`, a diffuse plane, flat
  numeric environment, scale `1e38`, and eight paths. CPU reflectance remained
  exactly `2^-126`; the device returned zero radiance while matched white-control
  paths implied `0.00987–0.01391`. This is explicitly outside a relative-radiance
  accuracy guarantee, despite meeting the absolute reflectance bound.
- Focused LLVM 18.1.8 clang-tidy with matching libc++ headers, all **46 Python
  tests**, formatting, and repository hooks passed. Negative oracle tests reject
  malformed/truncated/nonfinite output, changed saturation, excess error, and
  mismatched material/texture routes.
- The product CLI rendered an inspected 256x256 PNG at 256 spp from the file
  texture wrapper, with these preview overrides recorded in the staged input.
  Batches of seven committed all **16,777,216 samples**. Its SHA-256 is
  `f2901ee5230073e6efad6e06e02a498345b88b43389acba40b57c933c490dc74`.

The Release reference executable SHA-256 is
`c95c5a549de93a7bddd11a3439207c9f558e201e0270dcdccd598450b2fd1364`;
the unchanged Metal library is
`d133c9ac81776189bfb986c689949aeb860b86758e7e821093a473ddc5a45bd4`.
The clean pinned PBRT executable is
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`;
the separately linked reflectance probe is
`c7a6fed869fa701bc667281c1afd2cd0da6d084b60f68be30a4ce5d2f213516a`.
The rational-oracle dump is
`b33350fe60799dbe64d575f37afb6b3d10be20f6080b46e4f49831c477e7d0ad`;
the PBRT and translated named dumps are
`f39f60ef36cabd58a901dea55dea0c574a2977497aefb76b971d7dee7c255504`
and `4e8b6912fe93196049020ab050bf623dfec1137bc57e4e7428338b39d4c90908`.
Reference source/dependency provenance remains in the
[reference build record](pbrt-render-validation.md#pinned-upstream-reference).
Hosted checks are reported separately on the PR. No performance claim is made.
