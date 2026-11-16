# Subdivision and inline bilinear mesh validation

This change extends PBRT geometry ingestion. Loop control meshes are refined and
projected using the pinned PBRT algorithm, then rendered as triangles; inline
bilinear meshes use the existing procedural patch backend. The transport
estimator, sampling dimensions, raw accumulation and checkpoint contracts are
unchanged. This is selected geometry coverage, not full upstream scene support.

## Protocol

The CPU reference is PBRT v4 `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`, with a
clean source tree. Geometry comparison uses an independently linked PBRT probe
and a separate rypath converter executable. Indices must match exactly. Position
components use `2e-6 * max(1, abs(reference))`; normal components use `3e-5 *
max(1, abs(reference))`. Nonzero normal vectors must remain nonzero, with relative
length error at most `1e-4` and normalized-direction chord error at most `2e-4`.
These thresholds were selected before candidate-oracle measurements. Analytic
boundary masks and topology tests provide evidence independent of the adaptation.

The image suite has twelve scene families, each using independent and Sobol
fastowen sampling at 8×8 pixels and 4,096 samples per pixel. Each backend receives
identical input bytes, except the explicitly recorded independent seed values:
rypath 0–15 and PBRT 16–31. PBRT uses `--render-coord-sys world --nthreads 1`.
Full-frame and four quadrant RGB seed means are gated at
`5 combined standard errors + 2e-5 * max(1, abs(reference))`. Pixels are not
independent statistical replicates. Missing/nonfinite outputs, failed processes,
reference ERROR diagnostics and absent positive full-frame signal fail the suite.
Thresholds must not be relaxed after inspecting candidate results.

Fixtures cover boundary and closed subdivision, level-zero projection, curved
textured/alpha patches, affine orientation, powered triangle/patch emitters and
homogeneous-medium illumination. Two credited excerpts from the pinned
contemporary-bathroom geometry exercise boundary edges and extraordinary vertices.
Their level-two face counts are 32,768 and 37,376. They retain upstream geometry
bytes; the surrounding scenes are owned wrappers. See the
[fixture manifest](../tests/fixtures/pbrt-render/mesh-shapes/README.md) for source
hashes, licenses and dependency closure. The original bathroom entry point is
not supported by this change.

The initial two-worker reference run terminated with SIGSEGV on volume seed 17,
without a diagnostic. The same input later completed under both worker settings;
the cause is unestablished. All 32 fixed volume reference inputs completed with
one worker, after which the complete suite was rerun with that execution setting.
The original failed process record is retained. No fixture, seed schedule,
sample budget or statistical threshold was changed to handle the crash.

```sh
python3 -m unittest discover -s scripts -p test_compare_mesh_shapes.py
python3 scripts/compare_loop_subdivision.py \
  --rypath build/release/rypath_loop_subdivision_dump --output-dir build/loop-oracle
python3 scripts/compare_mesh_shapes.py \
  --rypath build/release/rypath_pbrt_reference --output-dir build/mesh-shape-reference
python3 scripts/generate_mesh_shape_samples.py --output-dir build/mesh-shape-samples
```

## Integration acceptance

Portable tests cover typed parameters, inherited values, Include locations,
malformed topology, checked resource growth, exact tensor ordering, normals/UVs,
static instances, alpha, medium interfaces and per-generated-primitive power.
Geometry changes must affect canonical scene identity, while equivalent lowered
meshes and batch partitions preserve it. Serialized continuation must preserve
raw Film exactly under both samplers.

GPU evidence includes independent projected boundary geometry, front/back and
reflected emission, direct lighting, and existing adversarial triangle/patch
intersection coverage. Exact per-sample importer comparisons frame primary rays
strictly inside their planar fixtures: an initial Sobol sample landed on the
outer silhouette, where native triangles and procedural patches have different
floating-point acceptance. Silhouette/intersection evidence remains in the
independent geometry tests and matched image comparisons; the interior test
does not promise bitwise equivalence at geometric boundaries.
A separate untouched `42c3079` build supplies legacy
regression outputs: 69 existing scenes, including area-power and configurable
box-filter cases, plus the demo checkpoint payload. These must remain byte-exact.

Supported Debug, Release and ASan/UBSan presets, portable compilation, Python
tools, formatting/static analysis and strict Metal API validation are required.

## Measured evidence

Implementation and test revision: `ff81056`, 2026-11-16, based on `42c3079`.
Hardware: Apple M4 Max; macOS 26.6 (25G5065a), Xcode 26.6 (17F113),
CMake 4.0.3 and Ninja 1.13.2. Portable checks use Clang 18.1.8 with Metal
disabled. Timings below are validation costs, not renderer benchmarks.

The final geometry oracle passed **36/36 cases** with exact triangle indices.
Maximum scaled component errors were `8.01e-9` for positions and `3.00e-8`
for raw normals; maximum relative normal-length error was `1.944e-6`, and
maximum normalized-direction chord error was `1.116e-6`. All remain below the
predeclared thresholds. The clean-revision report, independent probe, inputs
and per-process outputs are retained in
`build/mesh-loop-oracle-final/loop-oracle-jhaa0e0j`.
Three deliberate mutations (omitted limit projection, reversed normals and
incorrect boundary weight) were each detected by the portable oracle tests;
local records are in `.cache/mesh-shapes/mutations.json`.

The clean-revision image comparison passed **24/24 cases and 360/360 RGB
gates** in 79.92 s. The maximum absolute difference / threshold ratio was
**0.605417** (powered quad, independent sampler, top-left red). All 768 raw
PFMs, process records, seed measurements, fixture hashes and build provenance
are retained in `build/mesh-shape-reference-final/mesh-shapes-e9um_q1y`.
The original two-worker reference failure remains at
`build/mesh-shape-reference/mesh-shapes-_4bwwkig/quad-volume-independent/pbrt/17.process.json`;
the separate 32-input single-worker check is recorded in
`build/mesh-volume-single-thread-reference/summary.json`.

| Artifact | SHA-256 |
| --- | --- |
| Release `rypath_pbrt_reference` | `fc45e18fadb09b7ebd2c1adc25f58e1bed05b819a14f004953dcd3544e248de4` |
| Release `rypath_loop_subdivision_dump` | `746c28822febdff78f3f09a49ef603f9d010448b0b2ace0656ce261e9515d0ad` |
| Release `rypath.metallib` | `9b902e19b4b5aeaaa3813dd65a80ced24a43e50881fd4a6b6b41c3308b734d4f` |
| Pinned CPU PBRT | `4fd27e940f295fdb14f8407dbad7ef2e05dba9e9e2980c4a5cc3cad3da9b43b2` |

All **69 legacy scene PFMs** and the demo raw checkpoint payload are byte-exact
against the untouched `42c3079` build. The reports under
`.cache/mesh-shapes/{baseline,candidate}/report.json` retain input and output
hashes. The measured candidate renderer and shader hashes match the committed
implementation's Release artifacts.

The complete native Release and ASan/UBSan suites each passed **180/180**,
with no skips, in 149.14 s and 89.64 s respectively (four CTest workers).
Portable Clang 18 passed **137/137** in 11.91 s. Debug passed **178/178** in
332.54 s with the two new integration targets excluded; both new integration
tests also passed in focused runs.
The required pre-push hook additionally configures, builds and runs the complete
Debug suite. These logs are retained in `.cache/mesh-shapes/`.

All **176 Python tests** passed, as did all-file pre-commit checks and
Clang-tidy on the six changed project translation units. Apple static analysis
of `metal_backend.mm` and `metal_geometry.mm` completed without diagnostics.
Logs and the final static-analysis index are under `.cache/mesh-shapes/`.

Strict Metal API validation (`MTL_DEBUG_LAYER=1`, four CTest workers) passed
**45/45** tests without skips in 166.66 s, including the new mesh integration
tests. The full verbose log is `.cache/mesh-shapes/metal-api-tests.log`.
Metal shader validation was not enabled.

Independent review of the committed diff found no remaining material defect or
blocking acceptance gap. Combined unequal subdivision normals, nonuniform scale,
reflection, reverse orientation and direct lighting are exercised in components,
not in one upstream fixture. Existing normal transformation and shader paths
remain unchanged. This evidence establishes the selected geometry profile;
full upstream entry-point compatibility and broad convergence studies remain
separate acceptance work.
