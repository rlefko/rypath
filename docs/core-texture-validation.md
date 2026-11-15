# Core PBRT texture validation

This change extends the declared diffuse renderer with bounded PNG/JPEG inputs,
typed texture graphs, alpha visibility, surface detail, PLY displacement, and
image area/environment lights. It does not complete PBRT conformance or the
offline MVP. The [render profile](pbrt-rendering.md#core-texture-graphs-and-consumers)
defines the supported semantics and exclusions; [dependency notices](dependencies.md)
identify decoder and PBRT adaptations.

## Model, estimator, and numerical behavior

Texture graphs and image filtering deterministically evaluate the existing
spectral diffuse albedo. They consume no sampler dimensions. Raw signed spectral
operands are retained inside graphs; the diffuse root clamps reflectance to
[0,1]. Normal and bump maps alter the shading frame, while geometric normals
control offsets and emitter measures. PLY displacement changes the geometry
before acceleration construction. These are changes to the supported scene
model, not display approximations; raw XYZ accumulation remains independent.

Nonemitting alpha uses zero-footprint texture evaluation and deterministic
packed-ray hashing for fractional coverage. Native candidate traversal preserves
coincident geometry and both roots of a bilinear patch. Alpha on instances is
evaluated in the definition rendering frame, before placement. The hash agrees
with PBRT for identical packed tuples; ray transforms and rejected-root sequences
differ, so per-path identity with PBRT is not claimed. Emitting masks are certified
binary after image preparation. Constant-zero emitters remain invisible with
full direct-light weight; all-zero image masks are dark. Fractional emitting
alpha remains unsupported because pinned PBRT's separate hit and light masks
are inconsistent with its geometric MIS densities.

Area images retain geometric area/parameter sampling and the exact dyadic light
selection PMF. Infinite image lights use a mixture of 15/16 image importance
sampling and 1/16 uniform sphere sampling. The importance distribution uses
area-averaged RGB mass in at most 128×128 equal-area bins. Native-resolution
radiance is sampled independently. CDF intervals are stored in binary32; both
NEE and BSDF escape PDFs use their actual interval differences, including the
uniform component. Thus zero or rounded-away importance mass retains support.
Black images use uniform importance intervals. Both techniques use complementary
power weights; no delta events or new termination rules are introduced.

Finite direction conversion can move an edge sample into an adjacent PDF cell.
The sampler verifies its selected cell after the forward/world/inverse mapping
and makes a bounded inward adjustment when necessary. It then uses the same
PDF evaluator as an escaping BSDF ray; failure to recover the cell rejects the
pending batch. This is a finite-precision sampling adaptation, not exact equality
with the continuous estimator. In the recorded stress suite, 11 of 262,144
ordinary importance draws required adjustment; all 393,216 ordinary/targeted
proposals had matching sampled/evaluated PDFs, with maximum measured direction
change `3.19e-6`. Tests include native widths 4,096 and 4,093, a 127-cell NPOT grid,
non-axis rotations, concentrated texels, and transforms at the accepted Gram
matrix tolerance. Disabling the adjustment fails the cell-PDF control even with
the coarser grid.

Other declared numerical differences include stable Planck evaluation versus
PBRT's approximate `FastExp`, binary16 image storage, and zero-extension NPOT
black-wrap resizing (pinned PBRT aborts on that resize boundary). Coordinate-only
bilerp/dots accept defined spherical/cylindrical axis coordinates without requiring
undefined angular derivatives. Footprint consumers still reject undefined
required derivatives. A checker placed exactly on a spatial discontinuity can
expose different hit reconstruction rounding; the owned fixture manifest records
the observed boundary failure and the separate off-boundary comparison.

## Deterministic and native checks

The core runtime integration is recorded in commit `9e1789b`. Subsequent
coefficient-promotion and test-portability fixes preserve the supported model;
final branch checks are reported separately on the PR.

The implementation was exercised on Apple M4 Max with the repository's macOS 14 /
MSL 3.1 deployment contract. All checks use the actual device; portable builds
remain separate. These are implementation measurements, not evidence for other
GPU families.

| Check | Evidence |
| --- | --- |
| Static PNG | Independent owned encoder covers 60 Adam7 color/depth/shape combinations, exact decoded samples, malformed passes/indices, and budgets |
| JPEG | Five owned fixtures include grayscale/RGB, baseline/progressive, and mixed separate/interleaved DC scans; expected decoded bytes match the pinned stb reference |
| JPEG robustness | ASan/UBSan unit tests plus an independent 250,000-mutation harness; no sanitizer interruption; progressive coefficient padding is initialized |
| Graph CPU/Metal | 1,403 queries, 86 expected failures; maximum finite difference `7.7486e-7` |
| Graph vs PBRT | 6,440 comparisons across 40 features; maximum non-blackbody difference `5.811e-6`, blackbody difference `1.87576e-4` within the existing `2.5e-4` bound |
| Native alpha query | 32 closest/any-hit checks in two TLAS orders, including rejected near roots, finite intervals, coincident blockers and invalid callbacks |
| Alpha hash | 1,115 exact binary32 outputs against an independent packed-byte CPU oracle |
| Instance alpha | Both interior regions match pinned PBRT exactly: 48/48 and 56/56 black pixels; the translated region was 0/56 before the fix |
| Surface frame | Seven CPU/Metal cases include authored tangents, mirrored and nonuniform placement, geometric face-forward order, curved patches and large-tangent reduction |
| Image lights | Independent projection/PDF/radiance and frequency tests, 18 cell-frequency gates, gamma RGBA scalar-filter controls, and concentrated-cell roundtrip stress |
| Displacement | Analytic planes/normals, shared-edge refinement, shape-space metric, UV requirements, collapsed/nonfinite geometry and work/scratch/depth exhaustion |

CodeQL also identified coefficient products promoted to 64 bits only after
multiplication. Independent review confirmed that existing guards bound these
products within signed 32-bit range. Promotion now occurs before multiplication
on every coefficient path. Five owned JPEGs still match the pinned decoder byte
for byte; a further 50,000 deterministic mutations pass ASan/UBSan.

Independent review found and reproduced the instance alpha frame, nonuniform
surface orientation, angular-axis derivatives, repeated binary-mask scans,
failed-displacement work accounting, progressive JPEG padding, and environment
cell roundtrip issues. Each received a bounded fix and a regression check.

## Integrated reference comparisons

The [four owned core scenes](../tests/fixtures/pbrt-render/core-textures/README.md)
pass 48 statistical gates across 128 raw PFM outputs. They cover composite
mapped textures, fractional visibility, bump/normal maps, and visible/reflected
image lights. Each backend uses 16 fixed independent seeds at 8×8 and 4,096 spp.
The preselected gate is five combined standard errors plus
`2e-5 * max(1, abs(reference))`. No displayed PNG is used for these comparisons.

The existing 52-scene suite passes all 594 region/channel gates and eight
sensitivity controls across 1,672 raw linear outputs. It retains selected
external Cornell, PLY, and PNGSuite coverage. This is selected scene and asset
coverage, not validation of every entry point in the pinned scene collection.

All 20 Metal tests pass with API and shader validation enabled.
The integrated Metal tests also check exact graph controls, primary alpha counts,
shadow opacity, binary/invisible emitters, instance-local cutouts, normal-over-bump
precedence, directional surface detail, and dispatch/batch partition invariance.
A combined case enables graph albedo, alpha, surface detail, image emission and
environment lighting together and checks exact partition invariance.
For the recorded fixed workload, fractional-shadow residual was `5.69e-6`
(SE `9.95e-5`) and invisible-emitter residual `-1.95e-6` (SE `2.20e-6`).

The monolithic inlined evaluator caused an instrumented integrated pipeline to
time out during compilation at 180 seconds. Sharing its bounded evaluator across
shading, alpha, and bump callers completed the same strict test in 89.36 seconds,
including compilation. Warm runs are much shorter; this observation is a startup
limitation, not a throughput or time-to-quality speedup claim.

## Reproduction

Run the supported Debug, Release and ASan/UBSan presets described in
[contributing](contributing.md), and the separate portable build in
[Metal execution](metal-execution.md#build-and-ownership-contracts). Focused test names are `pbrt.png_mipmaps`,
`pbrt.core_texture_scene`, `render.texture_graph`, `geometry.displacement`,
`metal.texture_graph`, `metal.masked_intersection`, `metal.surface_detail`,
`metal.image_lighting`, and `metal.core_texture_render`. Set
`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` for strict native validation.

For the standalone graph oracle, build the pinned CPU PBRT binary32 reference
at `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`. This command uses the recorded
macOS reference build with Ptex and the upstream display dependencies enabled.
Other configurations must match the libraries on their PBRT executable link line.
The probe links PBRT only for this external check; rypath adds no PBRT runtime
dependency.

```sh
pbrt_source="$PWD/.cache/pbrt-reference/source"
pbrt_build="$PWD/.cache/pbrt-reference/build"
mkdir -p build/core-texture-reference
c++ -std=c++17 -ffp-contract=off -I"$pbrt_source/src" -I"$pbrt_source/src/ext" -I"$pbrt_build" \
  scripts/pbrt_texture_graph_probe.cpp "$pbrt_build/libpbrt_lib.a" \
  "$pbrt_build/src/ext/ptex/src/ptex/libPtex.a" \
  "$pbrt_build/src/ext/libdeflate/libdeflate.a" \
  "$pbrt_build/src/ext/double-conversion/libdouble-conversion.a" \
  "$pbrt_build/src/ext/utf8proc/libutf8proc.a" \
  "$pbrt_build/src/ext/glfw/src/libglfw3.a" \
  "$pbrt_build/src/ext/glad/libglad.a" \
  $(pkg-config --libs OpenEXR) -lz \
  -framework OpenGL -framework Cocoa -framework IOKit -framework CoreFoundation \
  -o build/core-texture-reference/pbrt_graph
cmake --build --preset release --target rypath_texture_graph_reference
python3 scripts/compare_pbrt_texture_graph.py build/core-texture-reference/pbrt_graph \
  build/release/tests/rypath_texture_graph_reference \
  --output-directory build/core-texture-reference/graph
```

The established full render comparison uses `scripts/compare_pbrt.py`; the core
fixture README gives explicit selection commands. Reference binaries and large
assets stay in ignored caches. [Performance measurements](core-texture-performance.md)
record complete render time and limits separately from correctness checks.
