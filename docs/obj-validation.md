# Polygonal OBJ/MTL validation

Implementation `5d0364c` was validated on 2026-11-15 on Apple M4 Max,
macOS 26.6 (`25G5065a`), Xcode 26.6 (`17F113`). The
[import profile](obj-import.md) defines supported inputs, material approximations,
numerical limits and explicit exclusions. This change affects ingestion,
geometric conversion and material-model selection. It does not change transport
estimators, sampling, raw accumulation or shaders.

## Portable contracts

- Loader tests cover independent and negative indices, omitted attributes,
  concave and reversed polygons, collinear corners, authored/generated normals,
  smoothing across group/material boundaries, relative libraries and source
  locations. Invalid indices, nonplanarity, self-intersection, degeneracy,
  narrowing loss, normal underflow and unsupported non-polygon records fail.
- Material tests inspect decoded derived texels and translate the resulting
  PBRT graphs. They cover legacy/PBR constants, color transfer, scalar channels,
  map transforms, roughness, opacity consistency, emission, bump and normal
  mapping, source snapshots, closure-dependent names and unsupported options.
  Native geometry/frame/emission limits are checked during preflight.
- Bundle tests independently decode emitted PLY attributes and indices, verify
  hashes and provenance, relocate bundles, and run PBRT preflight/translation.
  They exercise file/directory/symlink refusal, failed-stage cleanup, metadata
  escaping/UTF-8 validation, retained-memory and output limits. Directory/FIFO
  image sources are rejected before opening.
- CLI tests check modes, exit statuses and publication behavior. Source/fixture
  SHA-256 checks run during CMake configuration. No test executes historical
  OBJ commands or fetches a large external scene.

The portable orientation predicate was also compared against a Python rational
oracle on 20,000 generated binary64 coordinate cases during independent review; all signs
matched. That one-off review experiment is separate from the maintained CTest
fixtures. Retained-memory tests exercise the declared OBJ budgets, not a global
process-memory bound: staged PBRT validation has its existing separate budgets.

## Matched material rendering

`rypath_obj_render_tests` compares ten imported assets with separately authored
PBRT material graphs: diffuse, textured diffuse, glossy, conductor, dielectric,
opacity, bump, normal, emission, and a metallic/clearcoat mixture. References do
not read converter definitions or reuse derived converter images. Both sides
render through rypath's existing Metal renderer; this checks the declared mapping,
not physical equivalence between Phong and a BSDF or between two renderers.

Each case uses a 4×4 image, 512 spp, path depth 4, the CIE 1931 sensor and eight
independent randomizations. Imported seeds are `0x1823`–`0x182a`; reference seeds
are `0x1923`–`0x192a`. Raw spectral contributions are integrated into XYZ means
for four quadrants, giving 120 comparisons. The predeclared gate is

```text
abs(imported_mean - reference_mean)
    <= 6 * combined_standard_error + 2e-6 + 2e-4 * max(means)
```

Standard errors use seed-to-seed variation of region means. All comparisons
pass; the largest standardized difference is 3.16528 combined standard errors
(metallic/clearcoat case, quadrant 0, X). Additional same-seed bump/normal checks
pass the arithmetic floor alone, and paired negative controls detect removing
each map. These are bounded numerical fixtures, not a convergence or performance
study.

## Selected external asset

The unmodified TinyObjLoader cube OBJ/MTL closure is pinned at
`a6a5714eb144158373899faf2b899811a36d9fba`: two source files, 749 bytes, MIT.
The [fixture manifest](../tests/fixtures/obj/external/README.md), hashes and license
are checked in. It exercises six quad polygons, multiple groups and materials;
large upstream scene collections remain outside this acceptance scope.

The converted bundle successfully rendered with the pinned PBRT CPU executable
at `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f` and with rypath on M4 Max. A project-owned
wrapper at the bundle root supplies `LookAt 0 0 -4 0 0 0 0 1 0`, perspective FOV
45, a 16×16 RGB film, CIE 1931 sensor, box filter, independent sampler at 64 spp,
path depth 5 and an infinite light. The original asset is unchanged; the wrapper
supplies camera and lighting rather than claiming support for an upstream scene.
This is acceptance/renderability evidence, without a pixel-equivalence claim.

```sh
build/debug/rypath --convert-obj tests/fixtures/obj/external/cube.obj \
  --output build/obj-validation/external-cube
# Place the described scene.pbrt wrapper at the bundle root.
.cache/pbrt-reference/build/pbrt --nthreads 4 --quiet \
  --outfile build/obj-validation/external-pbrt.exr \
  build/obj-validation/external-cube/scene.pbrt
build/debug/rypath --render-pbrt build/obj-validation/external-cube/scene.pbrt \
  --output build/obj-validation/external-rypath.exr
```

## Check results

Debug, Release and ASan/UBSan each pass 127/127 tests. Metal-disabled LLVM 18
passes 95/95, and all 112 Python checks pass. LLVM 18 Clang-Tidy passes all nine
changed C++ translation units. Repository hooks and
independent review pass. Logs and local render artifacts are retained under
`build/obj-validation/` and are excluded from Git.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
# Repeat the configure/build/test commands with release and asan-ubsan.
ctest --test-dir build/portable-clang18 --no-tests=error
.venv/bin/python -m unittest discover -s scripts -p 'test_*.py'
```

Full-spec OBJ, additional MTL models, larger licensed corpus acceptance,
cross-GPU measurements and performance comparisons remain follow-up work.
