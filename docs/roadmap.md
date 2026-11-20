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
26. **Core PBRT texturing (merged in PR #27):** bounded PNG/JPEG inputs, typed graphs and procedural
    textures, alpha, bump/normal maps, PLY displacement, and image area/environment
    lighting with independent numerical validation.
27. **Linear HDR image I/O (merged in PR #28):** bounded HALF/FLOAT EXR
    and Radiance RGBE inputs, native binary32 texture/light storage, and atomic
    linear HALF/FLOAT EXR film output. See [HDR validation](hdr-image-validation.md).
28. **Spectral GGX conductors (merged in PR #29):** smooth and anisotropic reflection,
    raw optical spectra, scalar roughness textures, mapped shading frames, and
    independent BSDF/transport validation.
29. **Spectral GGX dielectrics (merged in PR #30):** smooth and anisotropic reflection/
    transmission, scalar/spectral eta, mapped frames, dedicated lobe sampling,
    and independent BSDF/transport validation.
30. **Extended PBRT surfaces (merged in PR #31):** finite coated diffuse/conductor,
    spectral measured and legacy Fourier tables, thin dielectric, diffuse
    transmission, nested mapped mixes and conductor optical textures. Explicit
    finite-layer and RGB-to-spectral approximations; bounded table loading and
    independent CPU/Metal/PBRT validation.
31. **Homogeneous spectral media (merged in PR #32):** absorption, scattering and
    absorption-coupled emission under explicit `volpath`, HG phase sampling,
    scoped interfaces, null boundaries and all 47 measured presets. Independent
    analytic/Metal/PBRT validation; heterogeneous media remain separate.
    The [bounded-fog reliability follow-up](bounded-fog-validation.md) replaces
    projected-progress rejection with certified null departure and preserves
    finite visibility endpoints.
32. **Broader PBRT rendering:** spectral point lighting adds a bounded all-point
    estimator for surfaces and homogeneous media, including geometry-free volumes.
    See [point-light validation](point-light-validation.md). Spectral spot lights
    extend the same surface/volume estimator with affine cones and cubic cosine
    falloff; see [spot validation](spot-light-validation.md). Spectral distant
    lights add PBRT finite scene-bound visibility and delta-direction surface/volume
    illumination; see [distant validation](distant-light-validation.md). Goniometric
    lights add native scalar angular diagrams to positional surface/volume lighting;
    see [goniometric validation](goniometric-light-validation.md). Projection lights
    add native RGB perspective images, affine scale/shear and light-space power
    normalization; see [projection validation](projection-light-validation.md).
    Positive diffuse area-light power adds per-primitive geometry/image
    normalization; see [area-power validation](area-power-validation.md).
    Perspective depth of field adds normalized uniform-disk thin-lens sampling,
    shared-aperture primary texture differentials and complete camera-support
    bounds; see [depth-of-field validation](depth-of-field-validation.md).
    Configurable box radii add normalized rectangular pixel footprints with
    matching lens/texture differentials; see [box-filter validation](box-filter-validation.md).
    Loop subdivision and inline bilinear meshes extend the existing triangle/patch
    geometry profile; see [mesh validation](mesh-shape-validation.md). Native sphere,
    disk and open cylinder, four-wavelength vacuum BDPT, finite film clamping and
    an explicit large-scene profile render the pinned Pavilion day/night scenes
    at their authored resolution and sample counts;
    see [Pavilion validation](pavilion-validation.md). Native light importance,
    directional proposals and pinned coating randomness are the current
    [sampling/convergence follow-up](sampling-convergence-validation.md); its
    original frozen study does not meet its quality target. Subsequent night
    attribution identified repeated Halton light splats; a full-Film light index
    corrects that reuse and reduces authored-clamped variance. Separate validation
    still fails the raw-tail and simultaneous quality targets. The subsequent
    [transport/intersection diagnosis](transport-regression-validation.md) adds
    the importance shading-normal adjoint and compensated native triangle
    candidate validation. Its completed renderer-18 study still fails all 48
    simultaneous quality bounds and ten night RGB mean safeguards, with a
    substantial measured slowdown. Mean/outlier diagnosis and performance
    recovery remain required; passing regression suites do not close these gaps.
    A subsequent fixed night PT comparison finds no regional rypath/PBRT mean
    failure, but within-renderer BDPT/PT differences remain, including in PBRT.
    Floor contribution attribution and repeated timing of the integrated
    arithmetic/visibility optimizations are the next validation steps.
    Curves and motion remain.
    Halton, ZSobol, Gaussian reconstruction and omitted PBRT descriptors extend
    camera sampling; see [sampling validation](sampling-defaults-validation.md).
    Wider color spaces, additional cameras, filters/samplers and faithfully
    rendered upstream entry points remain.
    JSON preflight reports remain follow-up work.
33. **Mesh import profiles:** polygonal OBJ/MTL merged in PR #34, with documented
    material approximations and portable PBRT bundles. STL (merged in PR #35) adds bounded
    ASCII/binary triangles, explicit vendor-color interpretation and Materialise
    coverage. Static glTF/GLB (merged in PR #36) adds selected-scene triangles and explicit
    metallic-roughness/texture/alpha rendering extensions; see the [glTF profile](gltf-import.md).
    Broader mesh profiles, glTF animation/extensions and further PLY variants remain.
    See [STL validation](stl-validation.md).
34. **Remaining surface models:** hair/curves, subsurface transport, arbitrary
    layered stacks and further lighting/MIS, with independent numerical comparisons.
35. **Resumable rendering (merged in PR #33):** exact raw checkpoints, compatible-build/device
    continuation, complete-batch cancellation and durable publication.
    Broader long-run accumulation studies remain acceptance work.
36. **Offline MVP acceptance:** licensed and pinned external scenes, convergence
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

Research candidates include multiple-scattering BSDFs, broader packet media
transport, improved sampling, and custom acceleration. Each needs a specific correctness
contract and measured benefit; none is an MVP prerequisite. FBX/COLLADA adapters
and USD scene composition require their own compatibility assessments.
