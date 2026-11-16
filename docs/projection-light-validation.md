# Projection-light validation

## Model and estimator

The [projection profile](pbrt-rendering.md#projection-lights) follows
[pinned PBRT construction and intensity lookup](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp).
Native RGB image values reconstruct an illuminant spectrum at the sampled
wavelength. The actual inverse affine declaration, with local Y reflection,
retains scale and shear. Local hither clipping precedes perspective division;
image support is inclusive and texel lookup is nearest with clamped endpoints.

Each eligible surface vertex adds
`throughput * f * abs(cosine) * I(lambda, direction) * Tr / distance_squared`.
A medium vertex substitutes its HG phase function for the BSDF/cosine factor.
All point, spot, goniometric and projection lights are enumerated once, within
the shared 64-source bound. Delta-position MIS weights are one. No new sampler
dimension or light-selection probability is introduced. Finite endpoint,
alpha, null-boundary and homogeneous shadow behavior reuse positional visibility.
Geometry-free media upload the same RGB and spectral resources; image-free
point/spot sky variants remain specialized separately.

Positive power retains PBRT's light-space cosine-cubed texel-center integral,
without an affine solid-angle correction. Binary64 integration is an explicit
numerical adaptation. Source image RGB is nonnegative normal binary32 or zero;
negative pixels and positive subnormals are outside this profile. Active inverse,
projection and RGB reconstruction arithmetic detects nonfinite results and lost
nonzero intermediates. Such failures reject the whole pending batch. These
checks declare a bounded floating-point model, not arbitrary-scale exactness.

Renderer contract 8 and identity schema 7 cover ordered positions, inverse maps,
image bindings/scales, projection coefficients and deep owned image contents.
Earlier renderer/identity contracts are rejected. File format, sampling and
accumulation remain version 1. Raw Film sums and counts remain independent of
PNG/EXR output conversion.

## Independent acceptance

Portable tests cover typed/inherited parameters, entry-relative owned assets,
RGB/scalar cache separation, rectangular images, PNG/JPEG/EXR/HDR decoding,
ignored alpha, image color-space independence from declaration ColorSpace,
FOV/numeric limits, exact inverse preparation, power integrals and combined limits.
Independent Python serializers reproduce prior identity and checkpoint goldens
before deriving the new ones.

Native tests use an independent double-precision projection oracle with CPU RGB
spectral reconstruction for asymmetric images and ordinary affine transforms. Separate binary32 cases
exercise exact/adjacent hither values, inclusive screen edges and nearest texel
ties. Declaration scale 500 illuminates the axis while scale 2000 is dark,
detecting incorrect normalization or common matrix rescaling. Tests cover point
equivalence, inverse-square attenuation, all four positional kinds, rough
reflection/transmission, occlusion, alpha, null boundaries, Beer attenuation and
bounded/geometry-free HG quadrature. Tiny normal RGB and extreme projection
arithmetic must fail without committing Film samples. Both samplers resume to
exact uninterrupted Film state across changed batch sizes.

The eleven [fixtures](../tests/fixtures/pbrt-render/projection-lights/README.md)
use 16 independent seeds per backend, 8x8 pixels, 4,096 spp and five RGB regions.
The fixed gate is five combined standard errors plus
`2e-5 * max(1, abs(reference))`. PBRT explicitly uses world coordinates.
Inputs, overrides, dependencies and binary hashes are retained. One owned wrapper
uses an unchanged pinned PNGSuite RGB asset with its original terms; this is
selected asset coverage, not support for an original upstream scene or full corpus.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
MTL_DEBUG_LAYER=1 ctest --preset debug --no-tests=error --verbose -R '^metal\.'
python3 scripts/compare_projection_lights.py \
  --rypath build/release/rypath_pbrt_reference --output-dir build/projection-comparison
python3 scripts/generate_projection_samples.py --output-dir build/projection-gallery
```

## Measured evidence

Production implementation `c3cd663`, with sample-harness correction `9ef0049`,
was validated on a 40-core Apple M4 Max, macOS 26.6 (25G5065a), Xcode 26.6
(17F113), and CMake 4.0.3. Debug, Release and ASan/UBSan each pass 174/174.
Debug was rebuilt by the pre-push hook on `9ef0049`; final Release and sanitizer
rebuilds also pass all five focused projection/identity/resume tests after the
test-only optional-guard cleanup. The Metal-disabled LLVM 18 suite passes
133/133, all nine changed-unit Clang-Tidy checks pass, and Apple Clang Static
Analyzer reports no findings for the Metal adapter. Python passes 144/144.
Repository hooks pass. Strict Metal API validation on `9ef0049` passes all
43/43 tests without skips in 330.26 seconds, including image-free point/spot sky
variants and projection/goniometric image access. The full log is
`build/projection-metal-api.log`.

All 165 region/channel gates pass across the eleven matched PBRT fixtures.
The largest difference uses 66.51% of its preselected bound (mixed lighting,
bottom-right region, blue). Results, individual seed values, staged inputs,
commands and artifact hashes are in
`build/projection-comparison/projection-at06_cy1`. The comparison binaries were
built from the production implementation before its commit; provenance records
`d992a48` with a dirty checkout. `build/projection-evidence.json` records the
committed-source hashes and retained binary hashes. The exact renderer binaries
used for the comparisons and gallery remain in `build/projection-rendered-binaries`.
The rypath comparison executable SHA-256 is
`4d77c4be03f74057ad8da590ba15c59093dc1d52a0ef45ad243eddb79515a22f`;
its metallib SHA-256 is
`7b027140af905bd10a303e5c8d00cd88785f14d57eff61aa3dd03dce75b57a64`.
The pinned PBRT executable SHA-256 is
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.

Eleven earlier environment, point, spot, distant, goniometric, conductor and
dielectric inputs retain byte-identical full raw PFM output against the saved
pre-projection release. Its source tree matches main `d992a48`. The script,
settings, dependencies, source state and binary hashes are retained under
`build/projection-baselines`; no sampling or renderer-control overrides are used.

Five isolated mutations fail their intended independent checks: bilinear instead
of nearest lookup, square instead of aspect-dependent bounds, direction
normalization before hither, halved photometric power and omitted Y reflection.
CPU and GPU baselines pass before mutation and after exact restoration.
`build/projection-mutations` retains outcomes, source manifests, mutated sources,
executables, metallibs and diagnostics. The initially stale RGBA assertion in
the copied CPU fixture is recorded separately; it was corrected to the actual
three-channel prepared image before the CPU mutation baseline and runs.

Independent implementation review found and resolved declaration-color-space
coupling and positive scale narrowing silently to zero. A separate documentation
review checked profile limits, links and attribution. No actionable findings
remain. Historical validation in earlier lighting records applies to its named
revisions, not to this implementation.

## Demonstration renders

Both generated scenes complete at 768x512 and 512 spp; the bounded-fog scene also
completes at 4,096 spp. The surface chart demonstrates RGB reconstruction,
nearest image detail, off-axis projection and affine shear. Fog demonstrates
the same image through homogeneous scattering and finite visibility.
These are undenoised renders with authored finite depth 6, Sobol fast-Owen seed
17 and batches of eight. They demonstrate integration, not convergence or
performance acceptance.

FLOAT EXRs, complete checkpoints and PNG previews published from those checkpoints
are retained in `build/projection-gallery` and `build/projection-gallery-4096`.
The independent checkpoint decoder confirms all 201,326,592 samples for each
512-spp image and all 1,610,612,736 samples for the 4,096-spp fog image.
`build/projection-gallery-evidence.json` retains input/output hashes, settings
and independent decoder results.

```sh
python3 scripts/generate_projection_samples.py --output-dir build/projection-gallery
build/release/rypath --render-pbrt build/projection-gallery/projection-studio.pbrt \
  --output build/projection-gallery/projection-studio.exr \
  --checkpoint build/projection-gallery/projection-studio.rycp --batch-size 8
build/release/rypath --render-pbrt build/projection-gallery/projection-studio.pbrt \
  --resume build/projection-gallery/projection-studio.rycp \
  --output build/projection-gallery/projection-studio.png
```

Use `projection-fog` for the volume example, and generate with `--samples 4096`
for its higher-sample version. Licensed original-scene corpus acceptance,
long-run convergence and M4 Max performance studies remain separate work.
