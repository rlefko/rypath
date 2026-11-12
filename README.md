# rypath

rypath is a C++20 spectral path tracer being developed for Apple Silicon through
Metal. The first goal is progressive offline rendering; a native macOS application
and interactive rendering follow later.

**Current state: progressive demo rendering and portable PBRT v4 preflight.** Metal traces a built-in
matte scene with native triangle/instance acceleration, environment lighting,
MIS, and compensated Russian roulette. Complete sample batches accumulate raw
XYZ; a separate display conversion saves the final PNG. `--check-pbrt` analyzes a declared scene profile and reports dependencies and
unsupported features. External-scene rendering remains in development.

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
./build/debug/rypath --render-demo --output build/demo.png \
  --width 256 --height 192 --samples 128 --batch-size 8 --seed 1
```

The Cornell preflight intentionally returns **2** for features outside the profile;
it creates no images and does not attempt rendering. Missing or malformed input
returns **1**; complete analysis within the profile returns **0**.

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

PBRT rendering translation, broader spectral materials, media, linear OpenEXR, and resumable
offline rendering follow this bounded demo. Scene compatibility, numerical
correctness, and performance each need separate evidence. The [PBRT profile](docs/pbrt-profile.md) distinguishes checked scene structure
from rendering conformance. There are no target-device performance claims yet.
Ray-tracing API flags describe device capabilities; geometry validation runs
through CTest, while the CLI smoke test exercises integer compute.

- [Architecture and product goals](docs/architecture.md)
- [Metal execution and validation](docs/metal-execution.md)
- [Native geometry and reference checks](docs/geometry.md)
- [Progressive rendering and numerical validation](docs/rendering.md)
- [PBRT preflight compatibility profile](docs/pbrt-profile.md)
- [Ordered PR plan and roadmap](docs/roadmap.md)
- [Contributing and validation](docs/contributing.md)
- [Current status](docs/status.md)
- [Dependencies and licensing](docs/dependencies.md)

## License and acknowledgment

Original project code is licensed under **GPL-3.0-only**; see [LICENSE](LICENSE).
Third-party code and scene assets retain their own licenses and attribution.

Development accelerated with OpenAI Codex.
