# Project status

## Current milestone

Polygonal OBJ/MTL, STL and static glTF/GLB profiles merged in PRs #34–36.
Spectral point, spot, distant, goniometric and projection lighting, plus the
bounded-fog repair, merged through [PR #43](https://github.com/rlefko/rypath/pull/43)
at `5642729`. The exact-tangent triangle emitter repair merged in
[PR #44](https://github.com/rlefko/rypath/pull/44) at `3731df7`.

Perspective depth of field adds normalized circular apertures and axial focus
for the existing PBRT perspective camera. It preserves pinhole raw output and
uses deterministic lens samples, matching primary texture derivatives and
checked binary32 aperture geometry. Identity 8, renderer 10 and sampling 2 reject
older checkpoints. See the [profile](pbrt-rendering.md#perspective-depth-of-field)
and [validation record](depth-of-field-validation.md).

The next bounded acceptance task remains inventorying supported unmodified
upstream entry points and their licensed dependency closures. Long-run
convergence and measured M4 Max benchmarks remain on the [roadmap](roadmap.md).
Offline MVP completion and full pinned-corpus coverage remain unestablished.

## Validation

Integrated depth-of-field implementation `5271ca5` passes 22 focused checks in
Debug, Release and ASan/UBSan, plus 17 portable checks and 151 Python tests.
Strict Metal API validation passes 10 relevant suites before integration and
four integrated camera/area/media/resume suites without skips. The earlier
candidate passed all 176 tests in each preset and all 134 portable tests. The
[record](depth-of-field-validation.md) distinguishes those revisions and retains
exact reference binary hashes.

All 24 pinned-PBRT lens cases pass 360 statistical gates. Fifty-two prior scenes
retain byte-identical raw PFM output against current main `3731df7`. Two intentional camera
mutations are detected. Tilted/translated ray and texture-derivative oracles,
high-index sampling, analytic absorption and exact checkpoint continuation pass.
Independent numerical and artifact review is complete. Representative renders
demonstrate focus changes; they do not establish convergence or performance.

Earlier lighting, media, material, importer, texture, HDR and checkpoint records
retain historical evidence for their named revisions.
