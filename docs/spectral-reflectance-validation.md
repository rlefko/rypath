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
