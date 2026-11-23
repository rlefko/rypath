# rypath

rypath is a C++20 spectral path tracer being developed for Apple Silicon through
Metal. Progressive offline rendering and an experimental native macOS viewer
are implemented; image-quality and interactive-performance qualification remain
open.

**Current state: progressive Metal rendering, a strict PBRT spectral profile, and
an experimental [native viewer](docs/viewer.md).** The viewer offers camera controls,
raw/reconstructed comparison and paired linear-RGB captures. Independent sampling
and signed temporal reconstruction are the fixture-tested defaults; Area/MCMC and
MetalFX remain explicit options. Native-1080p performance remains unqualified.

Render the built-in demo or PBRT files containing supported sampled spectra or
linear sRGB colors. Diffuse reflectance supports blackbody, built-in and file spectra,
typed texture graphs, and PNG/JPEG/EXR/Radiance HDR imagemaps.
Byte, binary16, and binary32 texture storage supports point, bilinear,
trilinear, and bounded EWA mip filtering. Floating inputs retain linear signed
and above-one samples, with pinned PBRT resampling and consumer semantics.
The PBRT profile includes inline triangles, external PLY triangles and bilinear
quads, per-vertex shading normals and UVs, constant environment lighting,
directly placed diffuse emitters and spectral point/spot/goniometric/distant lights
with sampled, RGB, blackbody, built-in named, or file emission.
[Goniometric lights](docs/pbrt-rendering.md#goniometric-lights) add native square
scalar emission diagrams with equal-area nearest lookup.
[Projection lights](docs/pbrt-rendering.md#projection-lights) project native RGB
images onto surfaces and fog, including nonuniform scale and shear. The profile also supports
box or triangle pixel filtering. Spectral conductors add polished and anisotropic
GGX metals with named/numeric/file optical constants and scalar roughness textures,
including bump and normal maps. Spectral dielectrics add polished and frosted
anisotropic glass with wavelength-dependent refraction and textured roughness.
Thin glass, diffuse transmission, nested material mixes, finite-depth coatings,
spectral measured tables and legacy Fourier tables extend this surface profile;
see the [material limits and validation](docs/surface-material-validation.md).
Explicit `volpath` adds homogeneous spectral absorption, scattering and emission,
Henyey–Greenstein phase functions, named medium interfaces and all 47 measured
PBRT presets; see the [media contract and validation](docs/medium-validation.md). Complete batches
accumulate raw XYZ; separate output conversion saves display PNG or linear
HALF/FLOAT OpenEXR. Portable `--check-pbrt`
reports scene structure, dependencies, and unsupported features. Broader PBRT
materials, instanced emitters, and other external assets
remain in development.

## Build and run

Requirements: CMake 3.25 or newer, Ninja, C++20 and C11 compilers. The foundation is
tested with Clang on Linux and Apple Clang on macOS. The Metal backend requires
macOS 14 or newer, full Xcode, and its Metal compiler tools. Apple Silicon is the
target platform. Linux builds the portable CLI with explicit Metal-unavailable
results.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
./build/debug/rypath --help
./build/debug/rypath --version
./build/debug/rypath --metal-info
./build/debug/rypath --metal-smoke-test
./build/debug/rypath --check-pbrt tests/fixtures/pbrt/cornell-box.pbrt
./build/debug/rypath --render-pbrt tests/fixtures/pbrt/cornell-box.pbrt \
  --output build/cornell.exr --batch-size 8
./build/debug/rypath --render-pbrt tests/fixtures/pbrt-render/image-magnified.pbrt \
  --output build/pbrt.png --batch-size 8
./build/debug/rypath --render-demo --output build/demo.png \
  --width 256 --height 192 --samples 128 --batch-size 8 --seed 1
```

The unmodified Cornell fixture passes structural preflight and translates with
its authored Sobol sampler. Preflight returns **0** for complete analysis within
the profile, **1** for missing or malformed input, and **2** for unsupported
features. It creates no images. The [PBRT render profile](docs/pbrt-rendering.md)
describes Sobol randomizations, limits, and differences in transport sampling.

PBRT EXR output honors `Film.savefp16` (default true: HALF; false: FLOAT).
The demo uses FLOAT for `.exr`; PNG keeps display encoding. Nonfinite output
and precision overflow fail without replacing an existing destination. The
[HDR contract and validation](docs/hdr-image-validation.md) defines the bounded
input formats, output metadata, and independent reference evidence.

`release` and `asan-ubsan` provide matching configure, build, and test presets.
The sanitizer preset requires Clang or GCC with AddressSanitizer and
UndefinedBehaviorSanitizer support. Build outputs stay under `build/`.

Metal is enabled by default on macOS. GPU tests require device access and fail
when a device is unavailable unless a CI run explicitly permits that condition.
See [Metal execution](docs/metal-execution.md) for toolchain setup, portable-only
builds and command results, and [geometry validation](docs/geometry.md) for the
ray-tracing contract and capability requirements.
The [rendering contract](docs/rendering.md) describes sampling, spectral units,
raw accumulation, output behavior, and validation limits. Rendering requires
compute ray tracing; Linux and Metal-disabled builds report it as unavailable.

## Direction

[Polygonal OBJ/MTL conversion](docs/obj-import.md) checks mesh assets and packages
PBRT world fragments with meshes, textures, and explicit material-conversion
warnings. Use `--check-obj MODEL.obj` or `--convert-obj MODEL.obj --output DIRECTORY`.

[ASCII/binary STL conversion](docs/stl-import.md) preserves flat triangle geometry
and supports VisCAM/SolidView and Materialise colors, with explicit convention
selection for ambiguous inputs. Materialise alpha maps to surface coverage.
Use `--check-stl MODEL.stl` or `--convert-stl MODEL.stl --output DIRECTORY`.

[Static glTF/GLB conversion](docs/gltf-import.md) preserves selected static triangle
scenes, core metallic-roughness inputs, native UV textures, signed normal frames,
sidedness and alpha through explicit rypath PBRT extensions. Use
`--check-gltf MODEL` or `--convert-gltf MODEL --output DIRECTORY`; scene selection
accepts `--gltf-scene INDEX`. Camera and lighting remain caller-owned.

[Checkpoint/resume and cancellation](docs/checkpointing.md) preserve complete raw
sample batches across compatible builds and supported Metal devices. Broader PBRT
rendering, remaining surface models and heterogeneous media follow this bounded profile. Scene compatibility, numerical
correctness, and performance each need separate evidence. The [PBRT profile](docs/pbrt-profile.md) distinguishes checked scene structure
from rendering conformance. Measured M4 Max traversal and scheduling results are
recorded in [frame rendering](docs/frame-rendering.md) and
[wavefront execution](docs/wavefront-rendering.md); complete-pipeline qualification
remains open.
Ray-tracing API flags describe device capabilities; geometry validation runs
through CTest, while the CLI smoke test exercises integer compute.

- [Native viewer: launch, controls and output modes](docs/viewer.md)
- [Frame reconstruction and its limitations](docs/reconstruction.md)
- [Architecture and product goals](docs/architecture.md)
- [Metal execution and validation](docs/metal-execution.md)
- [Native geometry and reference checks](docs/geometry.md)
- [Progressive rendering and numerical validation](docs/rendering.md)
- [PBRT preflight compatibility profile](docs/pbrt-profile.md)
- [PBRT spectral rendering profile](docs/pbrt-rendering.md)
- [Ordered PR plan and roadmap](docs/roadmap.md)
- [Contributing and validation](docs/contributing.md)
- [Current status](docs/status.md)
- [Dependencies and licensing](docs/dependencies.md)

## License and acknowledgment

Original project code is licensed under **GPL-3.0-only**; see [LICENSE](LICENSE).
Third-party code and scene assets retain their own licenses and attribution.

Development accelerated with OpenAI Codex.
