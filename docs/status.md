# Project status

## Current milestone

[Blackbody lighting (PR #17)](https://github.com/rlefko/rypath/pull/17)
is merged. The current slice adds all 88 pinned PBRT built-in spectra as emission
on supported infinite and diffuse area lights, using one shared name registry
and the existing checked spectral calibration. Named reflectance and file
spectra remain unsupported.

`--render-pbrt SCENE --output IMAGE.png` supports sampled spectra, linear sRGB,
blackbody, and built-in named lighting,
a pinhole camera, constant environment and directly placed area lights,
box/triangle filters, and independent or Sobol sampling. It honors finite depth,
retains calibrated raw XYZ and complete sample batches, and publishes a final PNG.
Portable `--check-pbrt` checks scene structure and supported image inputs without
Metal. The [render profile](pbrt-rendering.md) defines the subset.

Selected external coverage includes the unchanged Cornell scene, a PLY asset
in an owned wrapper, and an unmodified PNGSuite image in an owned wrapper.
Textured emission, wider color spaces, additional materials and importers,
OpenEXR, checkpointing, and the interactive app remain on the [roadmap](roadmap.md).
The next bounded compatibility task is to scope file-based emission spectra
and their dependency/size limits against pinned PBRT.

## Validation

The [named-spectrum record](named-spectrum-validation.md) records checks for
implementation `bd57579` and this PR's validation tests on Apple M4 Max:
71/71 tests in each of Debug, Release, and ASan/UBSan, 59/59 with Metal disabled,
and 14/14 with Metal API and shader validation. The pinned PBRT comparison
passed 450 statistical gates and two filter-sensitivity gates across 36 fixtures
and 1,154 linear PFM outputs. All 41,448 compiled spectral samples matched a
separately linked PBRT oracle exactly. Independent spectral/endpoint controls,
batch invariance, static analysis, 33 Python tests, repository hooks, and
independent review passed. Hosted checks are reported on the PR separately.

The [blackbody record](blackbody-light-validation.md) retains historical
evidence for PR #17. This remains selected coverage, not full-corpus support.
