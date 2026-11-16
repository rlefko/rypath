# Perspective depth-of-field validation

This change adds the normalized circular thin-lens model to PBRT perspective
cameras. It changes the camera integral and its sampling, while retaining the
existing spectral transport, finite-depth policy and compensated raw Film.
The [rendering profile](pbrt-rendering.md#perspective-depth-of-field) defines
supported parameters and limitations. Numerical tests provide bounded evidence,
not a proof of general PBRT equivalence or convergence.

## Model and sampling

The reference is the perspective camera in
[PBRT b0bdf461](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/cameras.cpp)
and the [projective-camera chapter](https://pbr-book.org/4ed/Cameras_and_Film/Projective_Camera_Models).
Uniform concentric disk sampling has area density `1/(pi * radius^2)`; the
normalized aperture integral gives unit camera weight. No aperture-area or
extra cosine factor is applied. Radius zero follows the original pinhole branch.

The independent sampler draws X/Y from Philox block `0xc0000000`; Sobol draws
dimensions 1022/1023 with the requested randomization. Neither consumes an
existing camera, wavelength, transport, dielectric, material or medium dimension.
Lens draws depend only on the pixel, absolute sample index and seed.
Identity schema 8, renderer contract 10 and sampling contract 2 reject earlier
incompatible checkpoints; file format and accumulation contracts remain 1.

Let `d` be the world transform of the unnormalized raster vector with camera
z=1, `C` the camera center, and `L` the sampled aperture offset. The shader forms
`O = float(C + L)`, `D = float(O - C)`, and normalizes `d - D/focaldistance`.
This scaled construction avoids a huge world-space focus point. It uses the
same realized `D` for primary and neighbor rays and for lens-relative texture
derivatives. Procedural mapping and indirect differential approximation retain
their camera-center reference. PBRT's minimum-differential search samples the
disk center, so the existing pinhole minimum-direction approximation remains
applicable.

World-origin quantization is an explicit binary32 limitation. A review
reproduction at camera `(1e7,1e7,0)`, radius 1 and focus 4 showed that mixing an
ideal offset with a rounded origin could amplify texture derivatives by about
42 times. The realized-displacement regression covers this case and rotated
frames. Host bounds include origin addition/subtraction rounding and its axial
projection, the complete pixel-filter support, positive normal parameters and
ratio, and independent aperture axes. Predictable failures precede dispatch;
nonfinite runtime origins/directions fail the complete batch.

## Validation protocol

Portable tests cover translation/defaults, inert zero-radius identity, authored
frames, malformed/range inputs and numerical support bounds. Native probes
exercise the production camera and primary-footprint helpers against independent
double-precision ray/plane equations, including all Sobol modes, high sample
indices, translated/reflected/rotated cameras and both pixel filters. Transport
checks cover constant-environment radiometry, geometry-free homogeneous media,
texture graphs, null boundaries, complete batches and exact continuation.

The [fixture manifest](../tests/fixtures/pbrt-render/depth-of-field/README.md)
records 24 preregistered cases: six focus/defocus/default/texture/Cornell scenes,
box and triangle filters, and independent and randomized Sobol sampling.
Each backend renders 8x8 pixels at 4,096 spp with 16 independent seeds:
rypath 0-15 and PBRT 16-31. Full-frame and quadrant RGB means must agree within
five combined standard errors plus `2e-5 * max(1, abs(reference))`.
Randomizations, rather than individual correlated Sobol pixels, are replicates.
The Cornell input is a credited derivative; it is not unmodified corpus evidence.

```sh
python3 scripts/compare_depth_of_field.py \
  --rypath build/release/rypath_pbrt_reference \
  --output-dir .cache/depth-of-field/comparison
```

The comparison tool retains exact inputs, closure hashes, executable hashes,
commands, diagnostics and raw linear PFM outputs. Presentation renders use
matched geometry/lighting/sampling at 768x512 and 512 spp, changing only radius
and focus. They are demonstrations, not convergence or performance results.

## Evidence

Integrated implementation `5271ca5` includes current main `3731df7`, the lens
implementation `1a753cb`, comparison tooling `b385763`, and the empty-sky binding
repair `b5628ec`. Validation ran on
Apple M4 Max (16 CPU / 40 GPU cores, 128 GB), macOS 26.6 (25G5065a), Xcode 26.6
(17F113), Apple Clang 21.0.0, CMake 4.0.3 and Ninja 1.13.2. Deployment remains
macOS 14 / MSL 3.1.

| Check | Result |
| --- | --- |
| Debug, Release, ASan/UBSan complete suites before main integration | 176/176 each, no skips |
| Integrated camera/sampling/checkpoint/area/CLI checks | 22/22 in each preset |
| Strict Metal API validation | 10/10 relevant suites before integration, then 4/4 integrated camera/area/media/resume suites |
| Portable LLVM 18 | 134/134 complete suite, then 17/17 final focused checks |
| Python tools | 151/151 |
| Changed C++ Clang-Tidy / native Apple analyzer / repository hooks | Passed |
| Pinned CPU PBRT | 24/24 cases, 360/360 statistical gates |
| Pinhole baseline against current main `3731df7` | 52/52 byte-identical raw PFM pairs |
| Intentional camera mutations | Both detected; original source restored and retested |

The final native lens suite checks 287,360 primary/neighbor/texture-derivative
samples, 380 exact lens/core random-word comparisons across sample-index carry
and high indices, and 16,384 absorbing-medium samples. Absorption uses the
renderer’s analog survival estimator: individual events match independently
computed free-flight thresholds, while means satisfy preregistered six-standard-
error Beer–Lambert gates. Constant environments retain exact radiometry across
aperture changes; all four Sobol modes and independent sampling retain exact
Film continuation. Disk checkpoint round trips cover textured surfaces and
geometry-free media.

The two mutations remove aperture-relative texture rebasing or substitute the
ideal displacement for the realized origin. Each compiles and fails the native
lens test. Independent review identified the large-origin defect before the
fix, inspected the final tilted-camera and absorption tests, reran the seven
comparison-harness tests and independently hashed all 104 baseline/current PFM
artifacts. No remaining findings were reported.

Complete-suite checks preceded final numerical-oracle refinements, the empty-sky
binding repair and integration of main. The focused checks above cover the
final camera, tangent-emission, sampling and checkpoint changes. The lens CTest
enables Metal API validation routinely; it catches the previously missing
auxiliary-buffer binding for a geometry-free camera with no active medium. All reference measurements use unchanged statistical thresholds.
The matched harness rejects zero signal from either backend as well as malformed,
nonfinite, incomplete or error-reported output. Artifact paths below are local
ignored caches, not required downloads or committed external scene assets.

- Comparison inputs, outputs and provenance: `.cache/depth-of-field/integrated-comparison/depth-of-field-yx0e0131/`.
- Baseline hashes: `.cache/depth-of-field/main-regression/report.json`.
- Mutation evidence: `.cache/depth-of-field/mutations/report.json`.
- Reproducible sample generator: `scripts/generate_depth_of_field_samples.py`.

At `5271ca5`, all 360 reference gates pass; the largest error/threshold ratio is
0.594411 (defocused triangle-filter Sobol, bottom-left blue). Both backends have
positive signal in every case. The retained hashes are:

| Artifact | SHA-256 |
| --- | --- |
| rypath Release reference executable | `31b5beaf010d08ed32729202700cd135ee34e1e1fca4c6393431d12736cebbf9` |
| Metal library | `954bfc557849f7afd3635457e7f9c226d9d1681477db6d773f71d795545b4959` |
| Pinned CPU PBRT | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |

Hosted CI and representative image completion are reported on the PR separately.
