# Project status

## Current milestone

[Indexed PNG textures (PR #25)](https://github.com/rlefko/rypath/pull/25) is merged.
The current slice adds noninterlaced 1-, 2-, and 4-bit grayscale PNG diffuse
textures. Packed samples expand exactly into the existing RGB8 texture path;
transparency keys preserve their grayscale values without compositing.

`--render-pbrt SCENE --output IMAGE.png` supports the declared spectral diffuse
profile, a pinhole camera, constant environment and directly placed area lights,
box/triangle filters, and independent or Sobol sampling. It honors finite depth,
retains calibrated raw XYZ and complete sample batches, and publishes a final PNG.
Portable `--check-pbrt` checks scene structure and supported inputs without Metal.
The [render profile](pbrt-rendering.md) defines supported semantics and limitations.

Selected external coverage includes the unchanged Cornell scene, a licensed PLY
asset in owned wrappers, and unmodified RGB, grayscale, and indexed PNGSuite
images in owned wrappers. Textured emission, wider color spaces, additional
materials and importers, OpenEXR, checkpointing, and the interactive app remain
on the [roadmap](roadmap.md). The next bounded compatibility task is to scope
Adam7-interlaced PNG diffuse inputs, including pass layout and resource limits.

## Validation

The [grayscale record](grayscale-texture-validation.md) records implementation
`89d8329` on Apple M4 Max: **74/74 tests** in each of Debug, Release, and
ASan/UBSan, **61/61** with Metal disabled, and **15/15** with Metal API and
shader validation, without skips.

Exact reference checks cover 13,836 source RGB codes, 41,736 within-backend mip
codes, and 396 selected pinned PBRT mip codes. Native RGB8 controls match 8,192
path radiances exactly. The full pinned PBRT suite passes **594 statistical gates
and eight sensitivity controls** across 52 fixtures and 1,672 linear outputs.
Independent malformed-input review, all 71 Python tests, changed-unit static
analysis, repository hooks, and an inspected CLI preview pass. Hosted checks are
reported on the PR separately.

The [indexed PNG record](indexed-texture-validation.md) retains historical
PR #25 evidence. This remains selected coverage, not full-corpus support or
offline MVP completion.
