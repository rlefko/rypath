# Tangent triangle emission validation

A high-sample prism demonstration exposed an existing area-light endpoint
failure. Both main `d992a48` and projection head `fb4e846` reject pixel 495111,
sample 1448 of the same 1200x800, 8192-spp Sobol scene. The failure follows a
diffuse floor bounce to a triangle emitter at their shared edge; it does not
originate in dielectric scattering or projection lighting.

The native hit is accepted at a tiny positive ray distance. Reconstructed
endpoint and previous proposal reference differ by `(0, -1.788139343e-7, 0)`
while the emitter normal lies along X. The endpoint cosine is exactly zero,
with finite nonzero throughput, area, distance squared and prior BSDF PDF.
Triangle MIS previously rejected this case; patches already assigned zero
light PDF and full BSDF weight. The fix applies that exact-zero convention to
both shapes. Positive-cosine arithmetic and its failure checks are unchanged.

The primary reference is pinned PBRT's
[Triangle::PDF](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/shapes.h#L1133),
whose area-to-solid-angle conversion maps an infinite tangent PDF to zero.
Rypath retains its uniform triangle-area proposal and existing ray offsets.
This repair does not claim arbitrary precision at touching surface junctions.
Renderer contract 9 rejects earlier checkpoints; identity schema remains 7.

A subsequent long run reaches a distinct case at pixel 839697, sample 3487:
the reconstructed endpoint and previous reference are bit-identical, so distance
squared is zero despite nonzero throughput. The renderer continues to reject
that unresolvable connection. The final demonstration mounts its light panels
clear of the floor and podium; it does not suppress this failure or change its
seed. Both failed attempts remain available with their exact scenes and logs.

## Reproduction and evidence

The native area test contains a reduced floor/emitter junction regression.
It checks the expected diffuse contribution `0.055 * 2 = 0.11`, complete Film
batch accumulation, the rough-conductor MIS branch and positive-cosine controls.
It exercises actual Metal intersections without glass or external assets.
Isolated old-guard, zero-tangent-weight and blanket-unit-weight mutations each
fail the relevant regression. Run it with the supported build presets:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error -R 'area|checkpoint|resume'
```

The original failed scene, log and saved-main reproduction are retained locally
under `build/prism-final/failed-attempt/`. Instrumented copies and exact-path
replays in `build/prism-debug/` identify the failing operands without modifying
production accumulation or selectively discarding samples. Independent
checkpoint serializers in `build/prism-debug/identity-contract/` reproduce the
old goldens before deriving the renderer-9 values.
The zero-distance reproduction is in `build/prism-final/failed-fixed-attempt/`;
mutation provenance is in `build/prism-debug/weight-mutation-results.json`.

The implementation patch based on `fb4e846` passes native Debug 174/174 tests
and Python 144/144 tests. Portable LLVM 18 passes 133/133 before the final
test-only additions; the final area-reference and checkpoint-codec targets
rebuild and pass individually. Clang-Tidy 18 passes all three changed C++ tests.
Current production-build and hosted-check results are reported on the PR.
Local transcripts are `build/prism-debug-tests.log`, `build/prism-python.log`
and `build/prism-analysis/`. These correctness checks are not performance
measurements or full-image convergence evidence.

## Prism demonstration

The demonstration views a white emissive slit through a closed, smooth 45-degree
prism using pinned `glass-F11` (upstream `GlassSF11_eta`) data. All illumination
is white. It models geometric-optics dispersion through ideal lossless glass,
without diffraction, polarization, absorption, denoising or exaggerated indices.

Independent triangle intersections and Snell refraction agree with the scene
generator within `2.77e-8` per direction component at 450, 550 and 650 nm.
All 471 central wavelengths from 360 through 830 nm have valid two-side-face
refracted paths, without total internal reflection or end-cap exits. The
source-hitting subset is unobstructed; Fresnel reflection remains sampled by the
renderer. This is central-ray validation, not a full-image convergence guarantee.
The constant-index control uses the same scene with eta fixed at its 550-nm
value, `1.7911193711417062`.
