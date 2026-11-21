# Extended surface material validation

This is the historical surface-material validation record. The later
[sampling follow-up](sampling-convergence-validation.md) replaces the coating
RNG policy described below and records current corresponding-route checks.

## Scope and references

This change extends the surface model and estimator, without changing raw Film
accumulation. It adds `coateddiffuse`, `coatedconductor`, `measured`, legacy
`fourier`, `thindielectric`, `diffusetransmission`, and nested `mix`, plus optical
spectrum textures and the conductor reflectance shortcut. The [render profile](pbrt-rendering.md#extended-surface-materials)
defines accepted parameters, limits and explicit approximations.

The PBRT v4 reference remains
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`. The relevant primary descriptions are
[finite layered scattering](https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/Scattering_from_Layered_Materials)
and [measured BSDFs](https://pbr-book.org/4ed/Reflection_Models/Measured_BSDFs).
Legacy Fourier behavior follows PBRT v3
`13d871faae88233b327d04cda24022b8bb0093ee` and its
[Fourier model](https://pbr-book.org/3ed-2018/Reflection_Models/Fourier_Basis_BSDFs).
The [v3 notice](../third_party/pbrt-v3/README.md) and existing
[v4 notice](../third_party/pbrt/README.md) identify adapted code and licenses.

## Estimator and numerical contract

Evaluated scattering, sampled continuation throughput, directional MIS density
and delta classification are separate. Thin-sheet transmission is straight
through, with summed internal Fresnel reflection. Diffuse transmission chooses
reflection or transmission in proportion to their scaled/clamped spectra and
samples the corresponding cosine hemisphere. Fourier transmission uses radiance
eta weighting. No new scene-wide medium stack is implied.

Layered samples retain their joint-path throughput; it is generally incorrect
to replace that ratio with ordinary `f*cos/directional_pdf`. The pinned finite
`maxdepth` and `nsamples` controls are honored and reported as a compatibility
approximation, separately from emergency arithmetic/execution failures. An
uncapped strict layered solution is deferred. Internal absorption and HG
scattering are part of this model, including coating-relative substrate optics.

Material-local Philox v1 streams use absolute sample indices, stable material
IDs and a separate counter domain, preserving existing outer dimensions. Variable
walk lengths cannot consume another pixel's or sample's outer dimensions. Mix
graphs are checked before rendering and resolved once per hit, before the chosen
child's bump/normal frame is prepared.

Directional layered PDFs use a fixed density-only RNG stream, independent of
transport/value event seeds. This is an adaptation of PBRT's stochastic PDF
approximation. Review found that event-specific density seeds broke MIS
complementarity. Hashing floating directions instead amplified a `2.98e-7`
rotated-frame rounding difference into density ratios up to 18.94. A fixed stream
removed that amplification in 10,722 accepted paths while preserving independent
transport/value sampling. Tests cover exact density agreement, complementary
weights, two-sided flips and rounded orthonormal frames. These checks establish
numerical consistency, not a proof of an unbiased finite layered model.

RGB optics use unbounded reconstruction; albedo RGB parameters use bounded
reconstruction. File/sampled diffuse-transmission reflectance stays raw until
multiplication by scale and the final consumer clamp. Tests explicitly distinguish
file reflectance 2 with scale .25 (result .5) from an erroneous early clamp (.25).
A texture shared between optical and albedo consumers receives separate compiled
roles. Three-channel Fourier data reconstructs spectra after angular RGB
evaluation, without an albedo clamp; one-channel tables remain achromatic.

## Bounds and malformed inputs

Readers enforce 128 MiB per input and 512 MiB aggregate working storage. Tensor
fields have checked types, shapes, offsets, non-overlap, products, finite values,
wavelength/angular grids, distributions and canonical packed offsets. GPU uploads
are checked against both the material-buffer budget and device limits. Repeated
references share immutable table storage. Measured distributions retain checked
conditional and marginal CDFs, interpolated over the same angular parameters.
CDF cache contents are validated against the normalized densities; complete
canonical storage and peak decode scratch are bounded before allocation. Tensor-v1 supports spectral reflective
isotropic and full-azimuth anisotropic data; reduced domains fail explicitly.

SCATFUN v1 accepts one basis and one or three channels. Duplicate zero zenith
knots preserve the reflection/transmission boundary; inactive rows may have zero
mass. Monotonicity and integrated conditional CDF consistency are checked at the
scale of the data. All-byte truncation, malformed dimensions, oversized products,
invalid distributions, offset mutations and source-located dependency failures
have portable tests. Nonnegative stored coefficients do not prove positivity of
every continuous Catmull–Rom/Fourier interpolant; runtime finite checks and PBRT's
angular nonnegative-value behavior remain necessary.

Review found brightness-dependent absolute tolerances in the inherited Fourier
inverse-CDF algorithm: an amplitude of `1e-8` could return only two azimuths with
a continuous PDF. Scale-relative inversion and analytic quantile residual tests
now cover amplitudes 1, `1e-8` and `1e8`, including curved zenith distributions and
angular harmonics. This corrects proposal numerics and intentionally need not
reproduce every direction from the legacy solver.

## Reference and asset coverage

Small owned tensor/SCATFUN fixtures are generated in routine tests; they are
format/transport fixtures, not physical measurements. The committed
[external asset manifest](../tests/fixtures/pbrt-render/surface-materials/external-tables.json)
records exact hashes, sizes, sources and terms. Assets remain in `.cache/bsdf-tables/`:

| Asset | Coverage and terms |
| --- | --- |
| `cm_white_spec.bsdf` | 6,951,224-byte isotropic spectral tensor from pinned v4 scenes `30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572`; RGL CC0 |
| `aniso_brushed_aluminium_1_spec.bsdf` | 111,463,352-byte full-azimuth anisotropic spectral tensor; hash-pinned RGL CC0 download |
| `roughgold_alpha_0.2.bsdf`, `paint.bsdf` | Archived v3 angular RGB tables; local validation only, no dataset redistribution; publisher terms and missing per-table license recorded |
| `roughglass_alpha_0.2.bsdf` | Explicit rejection coverage: 189 decreasing CDF entries, with local decreases up to about 2%; no silent repair |

The roughglass result is a real compatibility limit, not a rendering pass.
Fourier transmission is exercised with owned valid fixtures. The selected
measured wrappers do not establish support for the complete upstream scenes or
full pinned corpus. `scripts/validate_bsdf_tables.py` checks cached asset hashes
and both source revisions and performs no downloads.

## Current implementation evidence

Evidence below is from implementation commits `27fe231`, `2eb9ed2` and `460ea12`
on Apple M4 Max, macOS Darwin 25.6.0, Xcode 26.6 (17F113), CMake 4.0.3 and LLVM 18.
The last commit specializes legacy dispatch without changing scattering kernels,
sampling or the advanced-material path. Validation-only probes, reference scripts
and documentation follow those implementation commits.
Historical conductor/dielectric counts
in their own records describe earlier revisions.

The extended layered probe exercises 360,448 primary queries across 11 scenarios
and 22,528 directional-density queries against an independent double reference
and pinned PBRT. It includes absorbing/scattering slabs, conductor/diffuse bases,
finite-depth sensitivity, eta=1 rough/smooth flags, HG limits, thin-sheet limits,
and diffuse reflection/transmission. Value/throughput gates are seven combined
standard errors plus .003; proportional layered samples are never subjected to
an ordinary sample/PDF equality assertion.

Portable scene tests cover parameter precedence, entry-relative paths, immutable
snapshots, material graph bounds, optical/albedo texture roles and mapped child
selection. Integrated GPU tests cover all seven families under five sampler
modes, batch partition and threadgroup invariance, analytic transmission and
mix endpoints with child bump/normal maps. Debug, Release and ASan/UBSan pass 100/100 tests each; Metal-disabled Apple
Clang and LLVM 18 builds pass 74/74 each. All 94 Python checks pass. Logs are
retained as `build/surface-*-tests*.log` and `build/surface-python-final.log`.
The three native suites were repeated after `460ea12`, as was strict Metal API
validation (28/28, no skips); their logs use `surface-specialization-*-tests.log`.

All nine affected shader-instrumented suites pass on frozen Debug artifacts via
`python3 scripts/validate_surface_shaders.py`. This opt-in diagnostic gives each
test 600 seconds; ordinary CTest deadlines and numerical gates are unchanged.
It forces error reporting to stderr and abort-on-fault, following Apple's
[Shader Validation guidance](https://developer.apple.com/documentation/xcode/validating-your-apps-metal-shader-usage),
so a reported GPU fault cannot pass solely because a command buffer completes.
Instrumented render checks exceeded the normal 180-second deadline in initial
attempts, so those attempts are not passes. A preliminary extended-budget run
completed with shader validation enabled; the final isolated run adds forced
reporting, abort-on-fault and cleared inherited pipeline/check overrides.
All nine suites complete without skips or shader-validation errors, using warmed
pipeline caches. Commands, durations and outputs are retained in
`build/debug/surface-shader-validation/`. Builds must not replace the metallib
while these tests run. Relevant changed C++ units pass clang-tidy and the native
analyzer; repository formatting, handoff and commit checks also pass.

Table validation passes 18,259 synthetic GPU queries, including independent
cubic/azimuth quantiles and brightness invariance. Gold, paint and an owned
transmitting Fourier fixture each exercise 961 queries: independent double CDF
residuals must be at most `5e-5`, and identical-direction angular RGB/PDF agrees
with unmodified pinned v3 within the existing `2e-3` relative/absolute gate.
A separate GPU spectral probe checks 35 zero, neutral, chromatic and above-one
RGB cases against CPU evaluation and independently generated pinned PBRT
RGBUnboundedSpectrum values, including amplitude homogeneity and an output guard.
Both measured assets pass 961 queries at five wavelengths (400/475/550/625/700nm)
with `2e-4` direction and `2e-3` value/PDF gates against pinned v4.

`scripts/compare_surface_materials.py` passes six matched image cases (thin,
diffuse transmission, mix, coated diffuse, coated conductor and measured):
8×8, 4096 spp, 16 independent seeds per backend, 192 diagnostic-clean renders.
All 90 regional/channel gates pass the preselected five combined standard errors
plus `2e-5 * max(1, abs(reference))`. The largest difference is 4.07 combined
standard errors in coated-diffuse red. An independent decoder recomputed every
measurement and gate from the raw PFMs, verified staged-source equality apart
from seeds and checked all retained binary, shader, fixture and asset hashes.
Representative existing diffuse, rough-conductor and rough-dielectric PFMs are
byte-identical to a fresh Release build of main `a01bbf2`; this is a selected
regression check, not a universal floating-point identity promise. Results are
in `.cache/surface-regression/results.json`.

Artifacts: `.cache/surface-image-comparisons-final/surface-materials-2xx_3749/`.
Its provenance records the immutable executable/metallib and reference hashes.
Fourier is excluded from this v4 image comparison and validated separately above.
The final dispatch specialization at `460ea12` preserves byte-identical PFMs for
one retained seed in each of the six cases and a legacy diffuse fixture. This
representative check is recorded in `.cache/surface-specialization-regression/`;
the full 192-render comparison and showcases retain their `2eb9ed2` provenance.

## Measured lookup cost

The first real-table showcase exposed repeated O(width×height) summation in each
measured warp/inverse. Cached conditional/marginal CDFs now use binary interval
searches while preserving the piecewise bilinear model. Independent baseline/new
comparisons pass 4096 varied queries per real measured asset and 49 exact,
adjacent and endpoint plateau combinations, including valid zero-density events.

A matched M4 Max GPU command probe (961 queries, one warmup and seven measured
runs, same O2 shader/host configuration) recorded median 400.788 ms → .592 ms
for `cm_white` and 140.444 ms → .834 ms for aluminium. Cache overhead is 562,176
and 2,837,504 bytes respectively. These are lookup-probe timings, **not** a
complete-render or time-to-quality speedup claim. Evidence, before/after shaders
and raw runs are in `.cache/table-cdf-perf/`.

A separate complete-render check uses the owned measured plane, the unchanged
`cm_white` asset, 32×20 pixels, 16 spp, independent sampling, seed 42, maxdepth 4
and batch size 1024. Release `27fe231` and retained `2eb9ed2` artifacts each get
one warmup and three timed invocations through raw PFM publication. Median wall
time is .3970 → .0656 seconds; median process maximum RSS is 97.92 → 100.31 MiB
(not total GPU allocation). All 1,920 channels remain finite; maximum absolute
RGB difference is `1.65e-5`, relative RGB L2 difference `7.60e-7`, and maximum
difference/peak RGB `5.88e-6`. Both relative measures pass the preselected `1e-3`
gate. This establishes matched fixed-sample output for this small scene, not a
convergence or general time-to-quality claim. Sources, commands, hashes, build
and process memory measurements, raw images and timings are retained under
`.cache/table-cdf-render-perf/`.

## Reproducing showcases

Generate three owned studios, optionally binding the cached real tables:

```sh
python3 scripts/create_surface_showcase.py --output-dir .cache/surface-showcase \
  --measured-table .cache/bsdf-tables/cm_white_spec.bsdf \
  --fourier-table .cache/bsdf-tables/roughgold_alpha_0.2.bsdf
./build/release/rypath --render-pbrt .cache/surface-showcase/coatings.pbrt \
  --output .cache/surface-showcase/coatings.exr --batch-size 32
./build/release/rypath --render-pbrt .cache/surface-showcase/coatings.pbrt \
  --output .cache/surface-showcase/coatings.png --batch-size 32
```

Render `transmission-and-mix` and `tables` identically. Defaults are 640×400,
512 spp, fast-Owen Sobol, seed 42, outer maxdepth 8 and triangle filtering.
Coatings retain maxdepth 10/nsamples 1. FLOAT linear EXRs and display-sRGB PNGs
remain local in `.cache/surface-showcase/` with settings/provenance and output
hashes. All three final 640×400, 512-spp studios completed; EXR headers were
independently checked for seed, sample count, material stream version and model
depth settings. These are actual raw renderer outputs,
without denoising or retouching; images demonstrate behavior, while numerical
comparisons supply correctness evidence.
