# Spectral goniometric-light validation

## Model and estimator

The [render profile](pbrt-rendering.md#goniometric-lights) follows pinned PBRT's
[goniometric construction](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp)
and [intensity lookup](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.h).
A square scalar octahedral equal-area diagram modulates a spectral point source.
Each eligible surface vertex adds `throughput * f * abs(cosine) * I(w) * Tr / r²`;
a medium vertex replaces `f * abs(cosine)` with the HG phase function.
All point, spot and goniometric lights are enumerated once, sharing a 64-source
limit. Delta-position weights are one; no light-selection probability or new
sampler dimension is introduced. Existing continuous-light MIS is unchanged.

Finite connections retain the original source endpoint through receiver offsets
and null crossings. Alpha and medium transitions reuse the positional visibility
path. Geometry-free volumes upload and bind scalar images in the sky kernel.
Raw Film sums, compensation and sample counts remain separate from output encoding.

RGB diagrams average decoded channels in binary32, then round the mean into the
original image encoding/storage. PNG16 first rounds each decoded channel to HALF,
then rounds the mean to HALF. Grayscale sources preserve their values; alpha is
ignored. Derived scalar views have a separate cache key from native RGB views,
so using one file for multiple consumers cannot change another consumer's data.

Lookup uses the actual inverse declaration matrix followed by PBRT's Y/Z swap,
without matrix rescaling or direction renormalization. Nearest lookup clamps at
the square's boundary. Rotation/reflection and representable translation are
supported within the documented orthonormal tolerance; scale/shear beyond that
domain are unsupported. Rejecting negative samples and binary32-subnormal scalar
values is a deliberately narrower profile than pinned PBRT. Invalid connection
arithmetic fails the pending batch, without partial Film accumulation.

Positive photometric power uses the post-quantization map mean. Its accumulation
is widened to binary64 to avoid intermediate binary32 sum overflow; this is a
numerical adaptation, not a claim of identical PBRT rounding. A black diagram is
valid without positive power. Positive power requires a positive finite mean and
representable normalized intensity.

Renderer contract 7 and identity schema 6 include the ordered light records,
position, all spectral bins, inverse matrix, image binding and deep scalar image
data. Earlier renderer/identity contracts are rejected. Format, sampling and
accumulation versions remain unchanged.

## Independent acceptance

Portable checks cover source precision and scalar quantization, image cache
separation and byte accounting, inherited parameters and file spectra, source
locations, rigid/reflected transforms, actual inverses near the tolerance,
photometric power, shared limits and invalid inputs. Independent Python encoders
reconstruct the prior identity/codec goldens before deriving the new values.

Native tests compare constant diagrams with isotropic point lighting and use an
independent analytic equal-area oracle for asymmetric diagrams. Two additional
binary32 boundary cases distinguish the actual inverse from a renormalized
direction, including one within the translated declaration tolerance. Tests cover
inverse-square attenuation, additive lighting, spectra, rough reflection and
transmission, alpha/null visibility, bounded Beer attenuation, geometry-free HG
quadrature, zero-source sampling, and whole-batch failure. Both samplers retain
exact uninterrupted/resumed Film state across different batch sizes.

The nine [comparison fixtures](../tests/fixtures/pbrt-render/goniometric-lights/README.md)
use 16 independent seeds per backend, 8×8 pixels and 4,096 spp. The fixed gate is
five combined standard errors plus `2e-5 * max(1, abs(reference))`, applied to five
regions and three channels. PBRT commands explicitly select world coordinates;
inputs, dependencies, overrides and binary hashes are retained. The external-map
case is an owned wrapper around an unchanged pinned PNGSuite image, with its
original terms. It establishes selected asset coverage, not an original upstream
scene or full corpus acceptance.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
MTL_DEBUG_LAYER=1 ctest --preset debug --no-tests=error --verbose -R '^metal\.'
python3 scripts/compare_goniometric_lights.py \
  --rypath build/release/rypath_pbrt_reference --output-dir build/goniometric-comparison
python3 scripts/generate_goniometric_samples.py --output-dir build/goniometric-samples
```

## Measured evidence

Implementation `ef31a48` and execution repair `f2365a7` were validated on the
40-core Apple M4 Max with macOS 26.6 (25G5065a), Xcode 26.6 (17F113) and
CMake 4.0.3. Debug, Release and ASan/UBSan pass 172/172 tests each after the
repair. The Metal-disabled LLVM 18 build passes 132/132, and all 135 Python
checks pass on `ef31a48`; the subsequent repair changes Metal specialization
only. Changed C++ units pass Clang-Tidy 18, and the repaired Objective-C++ unit
passes Apple Clang Static Analyzer. Independent identity encoding reconstructs
the prior golden before deriving the new renderer-7/identity-6 bytes.

The final Release comparison passes all 135 region/channel gates across nine
fixtures, including bounded and geometry-free volumes and the pinned external
RGB diagram. The largest difference uses 47.53% of its permitted bound
(geometry-free volume, top-right region, red channel). The unchanged threshold
is five combined standard errors plus `2e-5 * max(1, abs(reference))`.
Final fixture, executable and metallib hashes, commands, process diagnostics
and seed measurements are retained in
`build/goniometric-comparison-resources/goniometric-j3_0lvpy`.
This run used the exact source later committed as `f2365a7`; comparison
provenance records the preceding `ef31a48` with a dirty checkout. The accompanying
`build/goniometric-resources-artifacts.json` retains the execution-diff hash,
source hashes and unchanged-artifact checks. The rypath comparison executable
SHA-256 is `57fd20c91f708bd66629332f16e13e018160af77af77e47159dc08dd2e01d7d4`
and metallib SHA-256 is
`66d5d4244c4ee86d7e6a145e1080120965ba13c2e382b97191fff90c49a26328`.
The pinned PBRT executable SHA-256 is
`4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2`.

Ten earlier environment, point, spot, distant, conductor and dielectric scenes
retain byte-identical raw PFM output against clean baseline `61ccba3`, whose
source tree equals main `1dc7b1f`. Both renderers use the same authored 8×8,
4,096-spp, independent-seed-0 inputs without overrides. The reproducible script,
source states, binary/metallib hashes and per-scene results are retained under
`build/goniometric-baselines-resources`.

Four isolated mutations fail the intended checks: reversing angular orientation
and replacing nearest lookup with bilinear each fail the asymmetric-map oracle;
renormalizing the transformed direction fails the actual-inverse boundary
regression; halving power normalization fails the white-map power integral.
Mutation sources, binaries and diagnostics are retained under
`build/goniometric-mutations`. The power comparison covers ordinary finite map
means; it does not establish PBRT rounding equivalence for the widened binary64
sum adaptation described above.

Strict Metal API validation initially exposed missing sky image-buffer bindings
in the earlier point/spot and resume paths. The repair specializes goniometric
image access separately, preserving image-free point/spot variants. An overlapping
pre-push rebuild then invalidated one executable path during validation; its
isolated rerun passes all 42 Metal tests with no skips in 293.98 seconds.
`build/goniometric-metal-api-isolated.log` retains that complete result separately
from the interrupted run. This includes image-free point/spot and resume paths,
active goniometric sky lookup, and existing environment-image paths.

Both generated scenes complete in the actual Release renderer at 768×512 with
512 and 4,096 spp, Sobol fast-Owen seed 17, batch size 8 and finite depth 6.
The surface scene demonstrates angular patterns, rotation, colored emission and
rough reflection; the bounded-fog scene demonstrates the same profiles through
homogeneous scattering and finite visibility. Each 4,096-spp render commits
1,610,612,736 samples. FLOAT EXRs, complete checkpoints, PNGs published from those
checkpoints, logs and input/binary hashes are retained in
`build/goniometric-gallery` (`build/goniometric-samples` for 512 spp).
These undenoised samples demonstrate integration; they do not establish
convergence or performance results.

Reproduce the gallery with the generator and render commands below; use
`angular-fog` in place of `angular-studio` for the second scene.

```sh
python3 scripts/generate_goniometric_samples.py \
  --output-dir build/goniometric-gallery --samples 4096
build/release/rypath --render-pbrt build/goniometric-gallery/angular-studio.pbrt \
  --output build/goniometric-gallery/angular-studio.exr \
  --checkpoint build/goniometric-gallery/angular-studio.rycp --batch-size 8
build/release/rypath --render-pbrt build/goniometric-gallery/angular-studio.pbrt \
  --resume build/goniometric-gallery/angular-studio.rycp \
  --output build/goniometric-gallery/angular-studio.png
```

Independent implementation and numerical review found no remaining actionable
issues, including a separate review of the resource-specialization repair.
Long-run convergence, original upstream-scene acceptance and M4 Max performance
benchmarks remain outside this bounded change.
