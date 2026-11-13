# Project status

## Current milestone

[Named constant textures (PR #14)](https://github.com/rlefko/rypath/pull/14)
are merged. The current slice adds named PNG imagemaps for diffuse reflectance,
including UV mapping and mipmapped bilinear filtering on inline triangles and
PLY triangles and bilinear quads.

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

## Validation

The [constant texture record](pbrt-texture-validation.md) retains evidence for
the merged foundation through PR #14. The [image texture record](image-texture-validation.md)
records the completed implementation checks on Apple M4 Max: 70/70 tests in
Debug, Release, and ASan/UBSan, 58/58 in Metal-disabled ASan/UBSan, and 14/14 with
Metal API and shader validation. The full pinned PBRT comparison passed all
348 gates across 27 fixtures and 864 linear PFM outputs. Hosted checks are
reported separately on the PR; this is selected coverage, not full-corpus support.
