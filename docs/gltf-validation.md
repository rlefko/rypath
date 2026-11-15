# Static glTF validation

Validation for the static glTF import change on Apple M4 Max, macOS 26.6 and
Xcode 26.6. This records this change; earlier validation documents describe their
own revisions. The [profile](gltf-import.md) defines fidelity and unsupported cases.

## Acceptance checks

Debug, Release and ASan/UBSan each passed 163/163 tests with real Metal execution.
All 112 Python policy/tool tests passed. Changed C++ units passed pinned LLVM 18
Clang-Tidy with its matching libc++ headers (the macOS 26 SDK headers require
newer builtins); changed Objective-C++ units passed the Apple static analyzer.
Repository formatting, dependency checksums, commit policy and handoff exclusion
checks passed. Linux and hosted macOS validation run separately in GitHub CI.

## Numerical evidence

The Metal BSDF probe passed 4,528 evaluation, PDF, sampling-weight and reciprocity
comparisons against independently written double equations, using absolute
1e-7 plus relative 2e-4 tolerance. Endpoint tests cover grazing directions,
zero/near-zero roughness, delta/continuous mixtures and rejected malformed inputs.
Positive roughness is never silently replaced with a delta lobe; arithmetic that
cannot represent the model fails visibly.

Eight energy/null-mass cases each use two independent seeds and 131,072 samples
(2,097,152 total). A separate hemisphere quadrature refines from 256 to 512 steps;
acceptance is four times its refinement difference plus five standard errors and
2e-4. Null events retain probability mass and are not resampled.

| Case | Double integral | GPU seed 1721 | GPU seed 89123 |
| --- | ---: | ---: | ---: |
| White metal, roughness 1, normal incidence | 0.306853 | 0.308622 | 0.306059 |
| Dielectric color .5, roughness .7, cosine .6 | 0.511854 | 0.512337 | 0.511797 |
| Mixed metal .45, color .3, roughness 1, cosine .3 | 0.251735 | 0.251868 | 0.251634 |
| White dielectric, zero roughness, cosine .05 | 1.71942 | 1.72014 | 1.71928 |

The final row deliberately exposes energy gain in the literal reference model;
it is not claimed to conserve energy in every configuration. Rough-metal loss
is also retained. RGB spectral reconstruction is our adaptation of the pinned
Khronos model, not recovered measured spectra or external-renderer image parity.

The GPU texture probe passed 36,311 cases against a separate double sampler:
all 108 wrap/minification/magnification combinations, POT/NPOT mip chains,
native top-left UVs, alpha, boundary taps, invalid ranges and derivative LOD.
Its tolerance is absolute 2e-6 plus relative 2e-5.

Full-render tests use eight independent seeds at 2,048 samples each. Their fixed
comparison tolerance is six standard errors plus .2% scale and a 2e-6 floor.
They cover geometric sidedness, MASK boundaries, stacked BLEND coverage,
signed/reflected tangent frames, normal scale, mapped backfaces, chromatic
factor-only materials, progressive partitioning and exact resumed raw films.
Equivalent PBRT/glTF area emitters agree at depths 1 and 3: white constant and
textured OPAQUE/MASK emitter differences were below 1.6e-9 in the measured CIE Y
means (tolerance about 5.6e-5). Reversed two-sided emitters and BLEND continuation
are included. Endpoint emission contributes before the finite-depth stop.

## Import and publication evidence

Portable tests cover scene selection, JSON/GLB framing, escaped keys and Unicode,
resource snapshots, checked strided/sparse accessors, selected semantic rejection,
unused structural validation, tangent generation and sharing, relocation,
provenance and atomic publication. CLI tests cover malformed and duplicate
options, scene selection, rejected conversions and unchanged existing outputs.
The tiny pinned Triangle and Box fixtures retain their notices and hashes;
they establish selected coverage, not full Khronos corpus acceptance.

Independent review additionally exercised reverse-index 80,000-node hierarchies,
100-million-operation sparse input, expanded instancing limits and repeat
publication with per-file hash comparison. These inputs now reject before
unbounded work or recursion. No external large asset was added to Git.

## Reproduction

Use the supported configure/build/test presets and the focused tests:

```sh
ctest --preset debug -R 'gltf' --output-on-failure
python3 scripts/create_gltf_showcase.py --rypath build/release/rypath \
    --output-dir .cache/showcase/gltf-run --samples 1024 --size 960 --render
```

The showcase generator produces original glTF/GLB geometry and textures, runs
actual conversion, then renders PNG and raw EXR independently from identical
settings. The samples use 960x720, 1,024 spp, Sobol fastowen seed 417 and maxdepth 8.
PNG receives display sRGB conversion; EXR retains linear floating-point output.
No denoising is applied. The textured study has an intentional checker texture
and normal-map grid; fine colored crosshatch at 64 spp was reduced by increasing
the sample count. Images are demonstrations, not numerical equivalence tests.
