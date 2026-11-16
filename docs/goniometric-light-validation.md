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
