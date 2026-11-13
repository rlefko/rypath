# Project status

## Current milestone

Foundations through [PLY rendering PR #13](https://github.com/rlefko/rypath/pull/13)
are merged, including the foundation audit in PR #12. The constant-texture slice
is ready for review: typed named constants, forward diffuse reflectance bindings,
captured RGB color spaces and shared spectral material payloads.

`--render-pbrt SCENE --output IMAGE.png` supports sampled spectra and linear sRGB,
diffuse triangle/bilinear instances, constant reflectance textures, a pinhole camera,
constant environment and directly placed area lights, box/triangle filters, and
independent or all four Sobol sampling modes. It honors finite depth, retains
calibrated raw XYZ and complete sample batches, and publishes a final PNG.
The [render profile](pbrt-rendering.md) defines its limits; portable `--check-pbrt`
never renders.

The unchanged pinned Cornell fixture remains a supported external entry point.
The small pinned PLY asset has owned wrappers and complete dependency/license
records; these do not establish full Sportscar scene or corpus support. Image
textures, wider color spaces, additional materials/importers, OpenEXR,
checkpointing and the interactive app remain future work.

## Current validation

The [constant texture validation record](pbrt-texture-validation.md) covers the
current implementation and fixtures. On Apple M4 Max (`applegpu_g16s`), macOS 26.6
(25G5065a), Xcode 26.6 (17F113), all 64 CTests passed in Debug, Release and
ASan/UBSan with both availability allowances disabled. All 12 native tests passed
Metal API and shader validation; 54 Metal-disabled sanitizer tests also passed.
All five changed C++ units passed LLVM 18 clang-tidy, and all 25 Python tests passed.

All 285 pinned CPU PBRT comparison gates passed across 736 linear PFM outputs,
with unchanged seeds and thresholds. Both texture/control pairs produce identical
material payloads and same-device raw films; all 64 corresponding PFM pairs from
the comparison run are byte-identical within each renderer. Independent review
and repository hooks passed. Earlier milestones retain their historical evidence
in their own records. No performance improvement is claimed.

Hosted Linux/macOS checks and CodeQL will run on the PR's final revision.
Hosted capability skips are separate from strict M4 Max validation. CodeQL covers
portable C++, not Objective-C++ or MSL.

## Next bounded task

Plan image-texture decoding and filtering as a separate PR after review of this
slice. Keep textured emission, broader imports and material/transport additions
separately scoped. See the [ordered roadmap](roadmap.md).
