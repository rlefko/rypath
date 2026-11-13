# rypath

rypath is a C++20 spectral path tracer being developed for Apple Silicon through
Metal. The first goal is progressive offline rendering; a native macOS application
and interactive rendering follow later.

**Current state: progressive Metal rendering and a strict PBRT spectral profile.**
Render the built-in demo or PBRT files containing supported sampled spectra or
linear sRGB colors, diffuse triangles, constant environment lighting, and directly
placed triangle emitters, with per-vertex shading normals, retained UVs, and box
or triangle pixel filtering. Complete batches
accumulate raw XYZ; separate display conversion saves the final PNG. Portable `--check-pbrt`
reports scene structure, dependencies, and unsupported features. Broader PBRT
materials, textured/instanced emitters, and external assets remain in development.

## Build and run

Requirements: CMake 3.25 or newer, Ninja, and a C++20 compiler. The foundation is
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
  --output build/cornell.png --batch-size 8
./build/debug/rypath --render-pbrt tests/fixtures/pbrt-render/rgb-multibounce.pbrt \
  --output build/pbrt.png --batch-size 8
./build/debug/rypath --render-demo --output build/demo.png \
  --width 256 --height 192 --samples 128 --batch-size 8 --seed 1
```

The unmodified Cornell fixture passes structural preflight and translates with
its authored Sobol sampler. Preflight returns **0** for complete analysis within
the profile, **1** for missing or malformed input, and **2** for unsupported
features. It creates no images. The [PBRT render profile](docs/pbrt-rendering.md)
describes Sobol randomizations, limits, and differences in transport sampling.

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

Broader PBRT rendering, spectral materials, media, linear OpenEXR, and resumable
offline rendering follow this bounded profile. Scene compatibility, numerical
correctness, and performance each need separate evidence. The [PBRT profile](docs/pbrt-profile.md) distinguishes checked scene structure
from rendering conformance. There are no target-device performance claims yet.
Ray-tracing API flags describe device capabilities; geometry validation runs
through CTest, while the CLI smoke test exercises integer compute.

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
