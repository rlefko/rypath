# Project status

## Current milestone

[Named lighting (PR #18)](https://github.com/rlefko/rypath/pull/18) is merged.
The current slice adds bounded plain-text spectrum files as emission on supported
infinite and diffuse area lights. Preflight retains shared checked knots, and
translation reuses calibrated light tables without rereading assets. Named and
file reflectance remain unsupported.

`--render-pbrt SCENE --output IMAGE.png` supports sampled spectra, linear sRGB,
blackbody, built-in named, and file lighting,
a pinhole camera, constant environment and directly placed area lights,
box/triangle filters, and independent or Sobol sampling. It honors finite depth,
retains calibrated raw XYZ and complete sample batches, and publishes a final PNG.
Portable `--check-pbrt` checks scene structure and supported image inputs without
Metal. The [render profile](pbrt-rendering.md) defines the subset.

Selected external coverage includes the unchanged Cornell scene, a PLY asset
in an owned wrapper, and an unmodified PNGSuite image in an owned wrapper.
Textured emission, wider color spaces, additional materials and importers,
OpenEXR, checkpointing, and the interactive app remain on the [roadmap](roadmap.md).
The next bounded compatibility task is to scope named and file-based diffuse
reflectance and constant-texture values against pinned PBRT.

## Validation

The [file-spectrum record](file-spectrum-validation.md) records checks for
implementation `87eb388` and this PR's validation tests on Apple M4 Max:
72/72 tests in each of Debug, Release, and ASan/UBSan, 60/60 with Metal disabled,
and 14/14 with Metal API and shader validation, without skips. The pinned PBRT
comparison passed 498 statistical gates and two filter-sensitivity gates across
40 fixtures and 1,282 linear PFM outputs. The independent spectrum oracle passed
1,884 samples, including 1,080 exact endpoint/zero checks. Spectral controls,
batch invariance, static analysis, 36 Python tests, repository hooks, and
independent review passed. Review also exposed and verified a fix for wide-interval
interpolation cancellation. Hosted checks are reported on the PR separately.

The [named-spectrum record](named-spectrum-validation.md) retains historical
evidence for PR #18. This remains selected coverage, not full-corpus support.
