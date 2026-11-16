# Area emitter power validation

Protocol fixed before rendering (2026-11-16); measured evidence is recorded below.
This changes the imported physical emission model; the transport estimator,
light selection and MIS PDFs are retained. Raw spectral accumulation remains
separate from presentation and no denoising is used.

PBRT v4 reference: `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`.
The reference source must be clean. Both backends receive identical scene bytes;
PBRT runs with `--render-coord-sys world --nthreads 2`. The harness retains
binary hashes, fixture/dependency hashes, actual commands, status, stdout,
stderr, linear PFM outputs and individual seed measurements.

The contract follows the pinned [PBRT area-light construction](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/lights.cpp#L910).
It is per generated triangle or bilinear patch: authored positive
power divides the existing photometrically normalized emission scale by
`pi * area * sidedness * imageMean`. Sidedness is two for two-sided emission
and one otherwise. Non-image spectra (including RGB L) use imageMean=1;
image emitters use the arithmetic luminance mean of the native RGB texels.
Geometry area is evaluated after transforms/displacement. Curved patches use
PBRT's fixed 3×3 area estimate, not an exact surface integral. No alpha or UV
area-distortion compensation is implied. Instanced emitters remain unsupported.

Each positive per-primitive normalization factor must be finite, positive and
normal binary32. Nonzero dense emission and image-scale products must remain
normal binary32; overflow, subnormal products and loss of a nonzero value are
diagnosed. Image power is folded into image scale before spectral evaluation,
avoiding an intermediate spectral product that could underflow before a large
factor restores it. Positive-power translation also diagnoses loss of nonzero
base dense spectra, RGB emission or authored scale; image validation rejects
lost RGB components during reconstruction. Explicit zero emission or scale
remains zero. These are deliberate numerical domain limits, not clamps or
replacement illumination. Checkpoint identity is 9 and renderer revision is 11;
sampling revision remains 2.

Seven owned wrapper families cover shared unequal-area triangles, affine and
two-sided emission, a curved patch, asymmetric native HDR RGB emission,
mixed-light MIS, homogeneous-volume direct lighting, and a licensed external
PLY wrapper. Numeric, named, file, blackbody and RGB spectra are represented.
Asset terms and complete dependency closure are pinned beside the fixtures.
This is selected coverage, not full PBRT scene conformance.

Each family uses both independent sampling and Sobol fastowen randomization,
8×8 pixels, 4096 spp and sixteen independent seeds per backend (rypath 0–15,
PBRT 16–31). Full-frame and four quadrant RGB seed means are gated separately
at `5 combined standard errors + 2e-5 * max(1, abs(reference))`. Pixels are not
statistical replicates. Missing output, nonfinite values, failed process status,
reference ERROR diagnostics, or a backend with no positive full-frame signal
fail validation. Thresholds must not be adjusted after inspecting measurements.

```sh
python3 -m unittest discover -s scripts -p test_compare_area_power.py
python3 scripts/compare_area_power.py --rypath build/release/rypath_pbrt_reference \
  --pbrt ../rypath/.cache/pbrt-reference/build/pbrt --output-dir build/area-power-validation
python3 scripts/generate_area_power_samples.py --output-dir build/area-power-samples
```

The generated 768×512, 512 spp showcase compares small and large two-triangle
panels under constant radiance and equal authored per-triangle power. The two
panels have equal triangle counts; the large panel has nine times the area.
Authored power is `pi * 0.32 = 1.00530965` per triangle, keeping the small
panel radiance matched across scenes while reducing large-panel radiance by nine.
The shared scale of 18 retains the existing emission scale contract.
The scenes use actual emitters, diffuse colored objects, floor and wall; render
each to separate linear EXR and display PNG outputs, without denoising. These
samples demonstrate appearance and are not statistical acceptance evidence.

## Measurements after PR #46

Source/tests `8ffb013` integrate area power onto merged main `cd6a55c`; runs
used documentation revision `499d2a5`. Only the status document conflicted.
Independent integration review verified that both features' source/test deltas
were preserved. Measurements used the same M4 Max, macOS 26.6 and Xcode 26.6
environment described below, on 2026-11-16.

| Check | Result |
| --- | --- |
| Metal API validation (`MTL_DEBUG_LAYER=1`) | 44/44 passed, no skips; 361.52 s |
| Focused Release / ASan-UBSan | 22/22 each; 31.40 s / 49.41 s |
| Metal-disabled LLVM 18 CTest | 135/135 passed; 18.36 s |
| Python tools in the project environment | 165 passed |
| Area-power PBRT comparison | 14 cases, 210/210 gates, 448 PFMs; 37.79 s |
| Box-filter PBRT regression | Eight cases, 120/120 gates, 256 PFMs; 16.78 s |
| Raw regression against merged main | 58/58 PFM files and demo checkpoint payload byte-identical |

Focused native presets cover translation, area/filter oracles, checkpoint
codec/identity/continuation, primary rendering and lens rays. The changed image
test also passed a fresh Clang-tidy check. These post-integration checks are
separate from the full pre-integration Release/ASan suites below. The required
pre-push hook configures, builds and runs the complete Debug suite.

Current comparison artifacts are under
`build/integrated-area-reference/area-power-oxn8z5ff` and
`build/integrated-box-reference/box-filters-hzraawg5`. Their maximum
error/threshold ratios are 0.536804 and 0.489013 respectively; all settings and
thresholds remain unchanged. Both suites used these SHA-256 identities:

| Artifact | SHA-256 |
| --- | --- |
| Release `rypath_pbrt_reference` | `88355aeea5cd4017fefda85d6711c00d305a6c2db2b26cd9fb32b9d45a95dbae` |
| Release `rypath.metallib` | `9b902e19b4b5aeaaa3813dd65a80ced24a43e50881fd4a6b6b41c3308b734d4f` |
| Pinned CPU PBRT | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |

Logs are `build/integrated-*.log` and `build/area-post-rebase-*.log`.
The durable checkout retains commands/statuses in
`.cache/renderer-followups/integrated-validation.json`, and direct input-closure,
raw-PFM and demo-payload comparison against `cd6a55c` in
`.cache/renderer-followups/integrated-baseline-comparison.json`.

Both 2,048-spp presentation scenes were rerendered with the combined binary.
Their PNGs are byte-identical to the previously shared samples. Current PNGs,
linear EXRs, checkpoints and render provenance are under
`build/area-power-samples-main46`; `comparison.json` records the PNG comparison.

## Measurements before integration

Implementation and test revision: `855566d` (parent `125b6ef`), 2026-11-16.
Portable, Python and static results below preceded the final image-test fixture
correction (`total_samples=37`, matching its existing sample budget); all three
native targets were rebuilt after the correction.

- Portable Clang 18: **135/135 passed**, including area power, importer,
  render identity and continuation tests (`build/area-portable.log`).
- Python: **163 passed**, including the twelve area harness tests
  (`build/area-python.log`).
- Clang-tidy: ten changed C++ translation units checked, with no unsuppressed
  findings (`build/tidy-*.log`). Native Clang static analysis of
  `src/metal_render.mm` completed without diagnostics
  (`build/area-native-analysis.log`).
- Independent final review found no remaining material issues; two earlier
  numerical findings were corrected and covered by tests.
- Metal API validation (`MTL_DEBUG_LAYER=1`): the full run passed **43/44**
  tests; the sole failure was the image fixture's missing total sample budget.
  After correction, focused `metal.pbrt_area` passed in **3.27 s**
  (`build/area-strict-recheck.log`). Thus all 44 API tests have passing evidence
  across the full run and focused recheck, not a fresh 44/44 single run.
  Shader validation was not enabled.

Earlier fixture preparation evidence: PBRT-only input preflight passed all
fourteen cases at one seed (16) and 4096 spp; artifacts:
`build/area-power-preflight/area-power-ki35319u`. This establishes valid selected
reference inputs, not backend agreement or a final-revision GPU result.

Matched PBRT comparison: **14/14 cases, 210/210 RGB gates passed** in
42.30 s on Apple M4 Max, macOS 26.6 (25G5065a), Xcode 26.6 (17F113),
CMake 4.0.3 and Ninja 1.13.2.
The largest absolute difference / threshold ratio was **0.536804**
(unequal triangles, independent sampler, top-right red). Artifacts:
`build/area-power-validation/area-power-1jx_qn3a`, including 448 linear PFMs,
per-seed measurements, summary, process records and full provenance. The dirty
state at revision `855566d` comprises documentation edits.

| Artifact | SHA-256 |
| --- | --- |
| Release `rypath_pbrt_reference` | `a63c5f66f53b29cf1dcbf01d3a5054f3c5ca578b266be26e358beceeb800a690` |
| Release `rypath.metallib` | `33919e7142b460f561dc55da80723bd34cc4fab3f1a2628c28f7c10086374d83` |
| Pinned CPU PBRT | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |

Regression checks: all **58 legacy scenes were byte-exact** against
`95ac4fc` (4.81 s), and the demo raw checkpoint payload was exact (0.40 s).
Reports are in the main checkout's `.cache/renderer-followups/legacy/area/report.json`
and `.cache/renderer-followups/demo/area.json`. The demo payload SHA-256 is
`1cce3b9fb2deca4f575907c276fb00572586a858b247faad071f814149b97dde`.

Both **768×512, 512 spp** showcase scenes were rendered without denoising,
saved as separate linear EXRs and display PNGs, and visually reviewed
(8.87 s total). Outputs and scene/binary/output hashes are retained in
`build/area-power-samples/render-provenance.json`; executable SHA-256 prefix
`8215bd0d46a4`. EXR SHA-256 prefixes are `fd803e7284b1` (constant radiance)
and `75b30fcd5165` (equal primitive power). PNGs were exported from the same
completed raw checkpoints. These samples demonstrate appearance, not
statistical acceptance.

The same scenes were also rendered at 2,048 spp for presentation while PR CI
ran. Their unchanged camera/exposure, exact commands, scene hashes and separate
PNG/linear EXR outputs are recorded in
`build/area-power-samples-clean/render-provenance.json`.

The complete native Release and ASan/UBSan suites each passed **177/177**, with
no skips, in 308.92 s and 364.64 s respectively (`build/area-release-full.log`,
`build/area-asan-ubsan-full.log`). Debug validation is additionally enforced by
the required pre-push configure/build/test hook. These timings are validation
costs, not renderer benchmarks. Wrapper agreement establishes only the selected
coverage described above, not conformance for the upstream Sportscar scene or
the full PBRT corpus.
