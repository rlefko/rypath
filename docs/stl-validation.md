# STL import validation

Implementation `9a1aaf6` was validated on 2026-11-15 on Apple M4 Max,
macOS 26.6 (`25G5065a`) and Xcode 26.6 (`17F113`). The
[import profile](stl-import.md) defines geometry, vendor colors, coverage,
resource limits and unsupported material records. This change affects ingestion
and material-model selection; it does not change transport estimators, shaders,
sampling, raw accumulation or checkpoint contracts.

## Portable contracts

- Decoder tests cover equivalent ASCII/binary triangles, multiple named/empty
  solids, CR/CRLF source locations, binary headers beginning with `solid`, raw
  normals/attributes and source-facet mappings. Malformed records, truncation,
  trailing data, nonfinite values, narrowing loss, determinant uncertainty,
  degeneracy and native surface-normal range failures produce errors.
- Color tests independently encode VisCAM and Materialise words, including
  inactive bits, channel endpoints/midpoints, object-color inheritance,
  ambiguous headerless words, conflicting modes, missing defaults and malformed
  header records. Alpha applies to both inherited and facet-owned RGB.
- Normal checks cover more than 100 zero/stale normals with aggregated warnings
  and finite `1e308` authored values. Geometry checks distinguish coordinates
  that narrow to zero from representable subnormal components in otherwise
  valid native geometry; the latter retain their original binary32 values.
- Bundle tests independently decode PLY coordinates and indices, verify hashes,
  color/coverage translation, solid/facet provenance and deterministic output,
  and relocate a complete bundle before PBRT preflight/translation. They exercise
  existing file/directory/symlink refusal and failed-stage cleanup.
- Resource tests cover input bytes, tokens, values, working/output storage and
  generated scene budgets, including the 254/255 mesh-expansion boundary and
  quoted material-token boundaries. CLI tests cover invalid/duplicate arguments,
  exit statuses, portable preflight and publication behavior.

Independent review found and resolved generated-token accounting, native
surface-normal range and CR-only location issues. A separate review probe checked
312 preparations across five custom-budget dimensions on one- and two-color
assets; all 78 successful preparations also published successfully. This one-off
probe is separate from the maintained CTest fixtures. No actionable review
findings remain.

## Matched rendering

`rypath_stl_render_tests` compares six imported binary assets with independently
authored PBRT triangle/material scenes: bent flat geometry, VisCAM colors,
Materialise default/facet colors, and Materialise alpha 0, 128 and 255.
References do not read generated converter definitions. Source-facet mappings
are checked separately from emitted color grouping.

Each case uses a 4×4 image, 512 spp, depth 4, CIE 1931, box filtering, and eight
independent randomizations. Imported seeds are `0x2823`–`0x282a`; reference seeds
are `0x2923`–`0x292a`. Raw spectral samples are integrated into XYZ means for four
quadrants, giving 72 comparisons. The predeclared gate is

```text
abs(imported_mean - reference_mean)
    <= 6 * combined_standard_error + 2e-6 + 2e-4 * max(means)
```

Standard errors use seed-to-seed region variation. All comparisons pass on M4 Max;
the largest standardized difference is 1.87609 combined standard errors.
Paired controls detect omitted sRGB transfer, ignored partial alpha and accidental
smoothing. Their thresholds were fixed before observing results. These tests
validate the declared conversion model through the existing renderer; they do
not establish spectral fidelity of vendor colors or refractive transparency.

## Selected external asset

The unmodified Assimp triangle is pinned at
`6bf4b3a4f0d1750c4a4cdc5a3dd0fef4ec0a666d`: one 176-byte ASCII STL,
with modified BSD-3-Clause attribution and checksum-verified license. Its
[manifest](../tests/fixtures/stl/external/README.md) records the full asset closure.

The converted bundle renders in both the pinned PBRT CPU renderer
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f` and rypath on M4 Max. An owned wrapper
at the bundle root supplies `LookAt 0 0 4 0 0 0 0 1 0`, perspective FOV 45,
16×16 RGB film, CIE 1931, FLOAT output, box filter, independent sampling at
64 spp, path depth 5 and an infinite light. The source STL remains unchanged.
This establishes selected asset renderability, without external-renderer pixel
equivalence or broader upstream scene coverage.

```sh
build/debug/rypath --convert-stl tests/fixtures/stl/external/triangle.stl \
  --output build/stl-validation/external-triangle
# Place the described scene.pbrt wrapper at the bundle root.
.cache/pbrt-reference/build/pbrt --nthreads 4 --quiet \
  --outfile build/stl-validation/external-pbrt.exr \
  build/stl-validation/external-triangle/scene.pbrt
build/debug/rypath --render-pbrt build/stl-validation/external-triangle/scene.pbrt \
  --output build/stl-validation/external-rypath.exr
```

## Check results

Debug, Release and ASan/UBSan each pass 142/142 tests, including strict Metal
execution. The Metal-disabled LLVM 18 build passes 109/109, and all 112 Python
checks pass. LLVM 18 Clang-Tidy passes all eight changed C++ translation units.
Repository hooks and independent review pass. Earlier OBJ and renderer records
retain their own historical counts; they are not evidence for this revision.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
# Repeat configure/build/test with release and asan-ubsan.
ctest --test-dir build/portable-clang18 --no-tests=error
.venv/bin/python -m unittest discover -s scripts -p 'test_*.py'
.venv/bin/pre-commit run --all-files
```

Logs and local render artifacts are retained in ignored `build/stl-validation/`.
This is bounded compatibility and numerical evidence, not a convergence study,
performance benchmark, or offline MVP acceptance claim.
