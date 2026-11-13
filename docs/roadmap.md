# Ordered PR plan

Each entry is a review boundary. Later entries may be split further as evidence
clarifies the work; completion is recorded in [status](status.md).

1. **Project foundations (merged in PR #1):** portable CLI, CMake/Ninja/CTest presets, development
   checks, compiled CodeQL analysis, licensing, public documentation, and protected
   review workflow. No renderer implementation.
2. **Metal capabilities and execution (merged in PR #2):** device/capability reporting, explicit
   resource and error handling, compiled MSL, and a minimal verified GPU dispatch.
3. **Native geometry and reference checks (merged in PR #3):** Metal triangle/instance
   acceleration, affine transforms and preserved winding, independent
   intersection/occlusion tests, and adversarial geometry cases. Shading-normal
   orientation follows with transport.
4. **First progressive image (merged in PR #4):** reproducible scalar-wavelength paths,
   diffuse environment NEE/MIS and roulette, complete sample batches, compensated
   raw XYZ accounting, and final PNG output with analytic/independent validation.
5. **PBRT scene preflight (merged in PR #5):** portable typed parsing, scene-state
   interpretation, checked inline geometry, dependency inventory, source-located
   terminal diagnostics, and a pinned CC0 fixture. No external-scene rendering.
6. **PBRT spectral rendering (merged in PR #6):** strict sampled-spectrum diffuse
   triangles, pinhole camera, constant environment, calibrated film and authored
   finite depth, with owned fixtures and pinned CPU PBRT comparisons.
7. **PBRT sRGB reconstruction (merged in PR #7):** continuous RGB diffuse spectra,
   constant RGB/D65 infinite lights, pinned offline reconstruction data, and
   independent spectral/CPU/Metal comparisons.
8. **Triangle area lighting (merged in PR #8):** scoped emission, finite visibility segments,
   matched light-selection/MIS densities, sidedness, and independent validation.
9. **Triangle pixel filtering (merged in PR #9):** normalized separable tent sampling,
   expanded camera-support validation, independent edge-footprint checks, and
   pinned CPU comparisons.
10. **Triangle attributes (merged in PR #10):** owned indexed normals/UVs, interpolated
    diffuse shading normals, geometric ray offsets and emitter sidedness, and
    independent analytic/reference validation.
11. **Sobol sampling (merged in PR #11):** all four PBRT randomizations, fixed dimensions,
    bounded absolute sample indices, CPU/MSL conformance, and the unchanged
    Cornell fixture rendered at its authored settings.
12. **Broader PBRT rendering:** external PLY meshes, wider color spaces, additional spectra,
    assets/textures, filters/samplers and faithfully rendered pinned upstream entry
    points. JSON preflight reports remain follow-up work.
13. **Mesh import profiles:** comprehensive polygonal OBJ/MTL, documented PLY
    variants, glTF/GLB and STL, conversion policies, and malformed-input fixtures.
14. **Spectral surfaces:** broader spectra and lighting/MIS, GGX conductor
    and dielectric scattering, and independent CPU/Metal numerical comparisons.
15. **Offline completion:** homogeneous spectral media, linear OpenEXR,
    checkpoint/resume, cancellation, and long-run accumulation validation.
16. **Offline MVP acceptance:** licensed and pinned external scenes, convergence
    checks, repeated M4 Max benchmarks, and measured performance bottlenecks.

The offline MVP is complete only when these capabilities work together and meet
their declared acceptance criteria. Large external assets remain outside Git;
acquisition manifests will record revisions, hashes, dependencies, sizes, and
per-asset licenses before opt-in downloads.

Before the next product slice, the current foundation audit tightens availability
skip checks, repository guards, and project agent guidance. It preserves the
renderer profile and the existing manual review boundary.

## After the offline MVP

Build the native accessible macOS application, then camera/scene interaction,
progressive refinement, and separately identified denoising or temporal previews.
Broader conformance includes full OBJ non-polygon geometry, additional mesh
formats, PBRT materials/cameras, curves, motion, and heterogeneous media.

Research candidates include multiple-scattering BSDFs, hero-wavelength packets,
improved sampling, and custom acceleration. Each needs a specific correctness
contract and measured benefit; none is an MVP prerequisite. FBX/COLLADA adapters
and USD scene composition require their own compatibility assessments.
