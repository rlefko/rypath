# rypath

rypath is a C++20 spectral path tracer being developed for Apple Silicon through
Metal. The first goal is progressive offline rendering; a native macOS application
and interactive rendering follow later.

**Current state: native geometry queries.** The backend builds Metal triangle and
instance acceleration structures and checks closest-hit and occlusion queries
against an independent CPU reference. The executable retains device diagnostics
and its compute smoke test. It does not render images or load scenes yet.

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
```

`release` and `asan-ubsan` provide matching configure, build, and test presets.
The sanitizer preset requires Clang or GCC with AddressSanitizer and
UndefinedBehaviorSanitizer support. Build outputs stay under `build/`.

Metal is enabled by default on macOS. GPU tests require device access and fail
when a device is unavailable unless a CI run explicitly permits that condition.
See [Metal execution](docs/metal-execution.md) for toolchain setup, portable-only
builds and command results, and [geometry validation](docs/geometry.md) for the
ray-tracing contract and capability requirements.

## Direction

The renderer will use Metal native acceleration structures, spectral Monte Carlo
transport, PBRT v4 scenes, and progressive raw accumulation. Scene compatibility,
numerical correctness, and performance will each have explicit evidence. No
rendering, scene-format, or target-device performance claims apply yet.
Ray-tracing API flags describe device capabilities; geometry validation runs
through CTest, while the CLI smoke test exercises integer compute.

- [Architecture and product goals](docs/architecture.md)
- [Metal execution and validation](docs/metal-execution.md)
- [Native geometry and reference checks](docs/geometry.md)
- [Ordered PR plan and roadmap](docs/roadmap.md)
- [Contributing and validation](docs/contributing.md)
- [Current status](docs/status.md)
- [Dependencies and licensing](docs/dependencies.md)

## License and acknowledgment

Original project code is licensed under **GPL-3.0-only**; see [LICENSE](LICENSE).
Third-party code and scene assets retain their own licenses and attribution.

Development accelerated with OpenAI Codex.
