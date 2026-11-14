# Project status

## Current milestone

[Point image textures (PR #22)](https://github.com/rlefko/rypath/pull/22) is merged.
The current slice adds EWA filtering for spectrum PNG diffuse textures, with
anisotropy from 1 through 16 and a default of 8. Portable and Metal lookup retain
signed elliptical footprints, wrapped mip transitions, bounded work, and
shared image storage. Metal preserves small nonzero mapped products through
anisotropy widening and center mapping.

`--render-pbrt SCENE --output IMAGE.png` supports the declared spectral diffuse
profile, a pinhole camera, constant environment and directly placed area lights,
box/triangle filters, and independent or Sobol sampling. It honors finite depth,
retains calibrated raw XYZ and complete sample batches, and publishes a final PNG.
Portable `--check-pbrt` checks scene structure and supported inputs without Metal.
The [render profile](pbrt-rendering.md) defines supported semantics and limitations.

Selected external coverage includes the unchanged Cornell scene, a licensed PLY
asset in owned wrappers, and an unmodified PNGSuite image in owned wrappers.
Textured emission, wider color spaces, additional materials and importers,
OpenEXR, checkpointing, and the interactive app remain on the [roadmap](roadmap.md).
The next bounded compatibility task is to scope 16-bit PNG diffuse inputs
against pinned PBRT, including decoded precision and mip storage.

## Validation

The [EWA record](ewa-texture-validation.md) records implementation `d71f547`
on Apple M4 Max: 74/74 tests in each of Debug, Release, and ASan/UBSan,
61/61 with Metal disabled, and 15/15 with Metal API and shader validation,
without skips. Native probes cover 39 EWA cases at four sample budgets.
Triangle, patch, secondary-hit, failed-batch, and exact batch-accounting
controls passed.

Independent review verified 400,192 native mapped products, 75,000 native
lookups, 22,500 portable stress cases, and 180,000 unchanged legacy lookups.
Discrete LUT-boundary differences remain explicit in the numerical record.
The full pinned PBRT comparison passed 546 statistical gates and eight
sensitivity controls across 46 fixtures and 1,480 linear PFM outputs.
All 55 Python tests, static analysis, repository hooks, an inspected CLI render,
and independent review passed. Hosted checks are reported on the PR separately.

The [point texture record](point-texture-validation.md) retains historical
evidence for PR #22. This remains selected coverage, not full-corpus support
or offline MVP completion.
