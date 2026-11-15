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
12. **PLY mesh rendering (merged in PR #13):** bounded external PLY decoding, triangles and
    faithful bilinear quads, native intersections, attributed diffuse surfaces and
    curved emission, with pinned asset and independent numerical validation.
13. **Constant texture rendering (merged in PR #14):** typed named constants, spectrum
    references in diffuse reflectance, and shared translated material payloads.
    See [constant texture validation](pbrt-texture-validation.md).
14. **PNG image texture rendering (merged in PR #15):** bounded image decoding, UV mapping and mipmapped
    bilinear filtering for diffuse reflection on triangles and bilinear quads.
    See [image texture validation](image-texture-validation.md).
15. **Trilinear image textures (merged in PR #16):** per-texture adjacent-mip filtering in linear RGB,
    shared pyramid storage, and pinned PBRT boundary semantics with independent
    portable/Metal checks. See [trilinear validation](trilinear-texture-validation.md).
16. **Blackbody light emission (merged in PR #17):** positive-temperature spectra for supported
    infinite and diffuse area lights, stable photometric normalization, and
    independent portable/Metal and pinned PBRT validation.
17. **Named light emission (merged in PR #18):** all 88 pinned built-in spectra on supported infinite
    and diffuse area lights, shared exact-name lookup, and checked calibration.
    See [named-spectrum validation](named-spectrum-validation.md).
18. **File light emission (merged in PR #19):** bounded plain-text spectrum inputs on supported
    infinite and diffuse area lights, immutable checked sources, and independent
    portable/Metal comparisons. See [file-spectrum validation](file-spectrum-validation.md).
19. **Named and file reflectance (merged in PR #20):** original built-in knots and checked signed file
    curves on diffuse materials and spectrum constants, with bounded GPU curve
    compilation and independent numerical validation. See
    [spectral reflectance validation](spectral-reflectance-validation.md).
20. **Blackbody diffuse reflectance (merged in PR #21):** continuous Wien-normalized spectra on
    diffuse materials and spectrum constant textures, with independent CPU/Metal
    checks and pinned PBRT comparisons. See
    [blackbody reflectance validation](blackbody-reflectance-validation.md).
21. **Point image textures (merged in PR #22):** signed point lookup at the floor mip level,
    shared image storage, and independent CPU/Metal and pinned PBRT validation.
    See [point validation](point-texture-validation.md).
22. **EWA image textures (merged in PR #23):** bounded anisotropic filtering on the shared PNG mip
    pyramid, faithful edge and mip transitions, and independent portable/Metal
    and pinned PBRT validation. See [EWA validation](ewa-texture-validation.md).
23. **16-bit PNG image textures (merged in PR #24):** source-precision decoding, binary16 mip storage,
    shared portable/Metal lookup, and independent pinned PBRT validation.
    See [16-bit PNG validation](image16-texture-validation.md).
24. **Indexed PNG image textures (merged in PR #25):** checked 1/2/4/8-bit palette expansion to RGB8,
    ignored palette transparency, bounded decoding, and independent PBRT validation.
    See [indexed PNG validation](indexed-texture-validation.md).
25. **Packed grayscale PNG image textures (merged in PR #26):** checked 1/2/4-bit sample expansion
    to RGB8, discarded transparency keys, and independent PBRT validation.
    See [grayscale validation](grayscale-texture-validation.md).
26. **Core PBRT texturing:** bounded PNG/JPEG inputs, typed graphs and procedural
    textures, alpha, bump/normal maps, PLY displacement, and image area/environment
    lighting with independent numerical validation.
27. **Broader PBRT rendering:** wider color spaces, additional
    spectra, texture filters and formats, filters/samplers, and faithfully rendered
    pinned upstream entry points.
    JSON preflight reports remain follow-up work.
28. **Mesh import profiles:** comprehensive polygonal OBJ/MTL, documented PLY
    variants, glTF/GLB and STL, conversion policies, and malformed-input fixtures.
29. **Spectral surfaces:** broader spectra and lighting/MIS, GGX conductor
    and dielectric scattering, and independent CPU/Metal numerical comparisons.
30. **Offline completion:** homogeneous spectral media, linear OpenEXR,
    checkpoint/resume, cancellation, and long-run accumulation validation.
31. **Offline MVP acceptance:** licensed and pinned external scenes, convergence
    checks, repeated M4 Max benchmarks, and measured performance bottlenecks.

The offline MVP is complete only when these capabilities work together and meet
their declared acceptance criteria. Large external assets remain outside Git;
acquisition manifests will record revisions, hashes, dependencies, sizes, and
per-asset licenses before opt-in downloads.

The foundation audit merged in PR #12, tightening availability checks, repository
guards, and project agent guidance. Large-corpus acquisition remains separately scoped.

## After the offline MVP

Build the native accessible macOS application, then camera/scene interaction,
progressive refinement, and separately identified denoising or temporal previews.
Broader conformance includes full OBJ non-polygon geometry, additional mesh
formats, PBRT materials/cameras, curves, motion, and heterogeneous media.

Research candidates include multiple-scattering BSDFs, hero-wavelength packets,
improved sampling, and custom acceleration. Each needs a specific correctness
contract and measured benefit; none is an MVP prerequisite. FBX/COLLADA adapters
and USD scene composition require their own compatibility assessments.
