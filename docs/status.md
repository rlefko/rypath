# Project status

## Current milestone

[PNG image textures (PR #15)](https://github.com/rlefko/rypath/pull/15)
are merged. The current slice adds trilinear filtering for diffuse PNG textures
on triangles and bilinear patches, with shared mip storage and pinned PBRT
boundary behavior. Bilinear remains the default.

`--render-pbrt SCENE --output IMAGE.png` supports sampled spectra and linear sRGB,
a pinhole camera, constant environment and directly placed area lights,
box/triangle filters, and independent or Sobol sampling. It honors finite depth,
retains calibrated raw XYZ and complete sample batches, and publishes a final PNG.
Portable `--check-pbrt` checks scene structure and supported image inputs without
Metal. The [render profile](pbrt-rendering.md) defines the subset.

Selected external coverage includes the unchanged Cornell scene, a PLY asset
in an owned wrapper, and an unmodified PNGSuite image in an owned wrapper.
Textured emission, wider color spaces, additional materials and importers,
OpenEXR, checkpointing, and the interactive app remain on the [roadmap](roadmap.md).
The next bounded compatibility task is to scope blackbody light emission against
pinned PBRT before expanding the spectral profile.

## Validation

The [trilinear record](trilinear-texture-validation.md) records current checks
for implementation `b7e53e5` on Apple M4 Max: 70/70 tests in each of Debug,
Release, and ASan/UBSan, 58/58 with Metal disabled, and 14/14 with Metal API and
shader validation. The pinned PBRT comparison passed 354 statistical gates and
two filter-sensitivity gates across 28 fixtures and 898 linear PFM outputs.
Independent controls, batch invariance, static analysis, 27 Python tests, and
repository hooks passed. Hosted checks are reported on the PR separately.

The [image texture record](image-texture-validation.md) retains historical
evidence for PR #15. This remains selected coverage, not full-corpus support.
