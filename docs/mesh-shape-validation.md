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
Measurements and exact revision/toolchain identities will be recorded below
after validation; the protocol itself is not a passing test result.
