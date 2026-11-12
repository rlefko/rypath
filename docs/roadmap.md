# Ordered PR plan

Each entry is a review boundary. Later entries may be split further as evidence
clarifies the work; completion is recorded in [status](status.md).

1. **Project foundations:** portable CLI, CMake/Ninja/CTest presets, development
   checks, compiled CodeQL analysis, licensing, public documentation, and protected
   review workflow. No renderer implementation.
2. **Metal capabilities and execution:** device/capability reporting, explicit
   resource and error handling, compiled MSL, and a minimal verified GPU dispatch.
3. **Native geometry and reference checks:** Metal triangle/instance acceleration,
   transforms and orientation, independent intersection/occlusion tests, and
   adversarial geometry cases.
4. **First progressive image:** reproducible spectral path state, complete sample
   batches, raw sample accounting, a small declared transport model, and a saved
   image validated against analytic or independent references.
5. **PBRT ingestion:** parser/dependency evaluation and license review, canonical
   scene data, source-located preflight diagnostics, a compatibility matrix, native
   triangle/PLY assets, textures, and the first pinned upstream scene entry points.
6. **Mesh import profiles:** comprehensive polygonal OBJ/MTL, documented PLY
   variants, glTF/GLB and STL, conversion policies, and malformed-input fixtures.
7. **Spectral surfaces:** wavelength/sensor weighting, lighting/MIS, GGX conductor
   and dielectric scattering, and independent CPU/Metal numerical comparisons.
8. **Offline completion:** homogeneous spectral media, linear OpenEXR,
   checkpoint/resume, cancellation, and long-run accumulation validation.
9. **Offline MVP acceptance:** licensed and pinned external scenes, convergence
   checks, repeated M4 Max benchmarks, and measured performance bottlenecks.

The offline MVP is complete only when these capabilities work together and meet
their declared acceptance criteria. Large external assets remain outside Git;
acquisition manifests will record revisions, hashes, dependencies, sizes, and
per-asset licenses before opt-in downloads.

## After the offline MVP

Build the native accessible macOS application, then camera/scene interaction,
progressive refinement, and separately identified denoising or temporal previews.
Broader conformance includes full OBJ non-polygon geometry, additional mesh
formats, PBRT materials/cameras, curves, motion, and heterogeneous media.

Research candidates include multiple-scattering BSDFs, hero-wavelength packets,
improved sampling, and custom acceleration. Each needs a specific correctness
contract and measured benefit; none is an MVP prerequisite. FBX/COLLADA adapters
and USD scene composition require their own compatibility assessments.
