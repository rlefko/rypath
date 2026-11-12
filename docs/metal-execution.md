# Metal execution

This slice establishes device diagnostics, offline shader compilation, resource
ownership, and a checked compute dispatch. It changes execution only. There is
no transport estimator, scene ingestion, acceleration structure, or rendered
image yet.

## Requirements and commands

The Metal backend targets macOS 14 or newer and MSL 3.1, with Apple Silicon as the
target hardware. Full Xcode and working Metal compiler tools are required. Xcode
26 installations may need the separate component:

```sh
xcodebuild -downloadComponent MetalToolchain
```

Build and test with the existing `debug`, `release`, or `asan-ubsan` presets:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --no-tests=error
./build/debug/rypath --metal-info
./build/debug/rypath --metal-smoke-test
```

`--metal-info` reports whether Metal was built and whether a default device is
available. For an available device it reports its name, registry ID, architecture,
unified memory, and separate compute/render ray-tracing API flags. The selection
policy is the system default, with no automatic substitution. This command
returns success for a normal unavailable result so it remains useful on Linux.

`--metal-smoke-test` returns `0` only after successful GPU completion and verified
readback, `2` for an unavailable backend/device, and `1` for execution failure.
Invalid or combined command options also return `1`. The `Metal status` line
distinguishes `success`, `not_built`, `no_device`, and `failure`.

Ray-tracing API support is not a verified hardware ray-tracing execution result.
The dispatch check can run without ray-tracing support. Device identity and API
capabilities are observations, not performance measurements.

## Build and ownership contracts

Metal is enabled by default on macOS. Missing compiler tools fail the build with
an actionable diagnostic. Other platforms build the C++ stub without Apple SDK
dependencies. An explicit portable-only build on macOS uses a separate directory:

```sh
cmake --preset debug -B build/portable -DRYPATH_ENABLE_METAL=OFF
cmake --build build/portable
ctest --test-dir build/portable --output-on-failure --no-tests=error
```

CMake compiles MSL ahead of time, applying the macOS deployment target at both
compilation and linking. Shader outputs have their own dependencies and use a
build-local module cache. The generated `rypath.metallib` stays beside the
executable. Runtime lookup resolves the executable's canonical path, so launching
from another directory or through a symlink does not change the asset location.
Keep the executable and library together when copying build artifacts.

The macOS adapter links Metal, Foundation, and CoreGraphics and keeps Apple types
behind a private C++ interface. ARC retains the device, queue, pipeline, command
buffer, and shared buffers through completion. CPU reads occur only after the
command buffer reports successful completion. Failures identify the relevant
stage and include the available Metal error description.

## Execution validation

The fixed smoke workload has logical lengths 1 and 257. For index `i`, the CPU
initializes an unsigned 32-bit input to `13 + 5*i`; the shader computes
`3*input[i] + 17 + i`. The CPU checks the independent equivalent `56 + 16*i` for
every logical output. These small values avoid overflow. Output buffers begin
with sentinel values. Threadgroups are rounded up, the shader guards its logical
range, and every padded guard element must remain untouched.

The test checks two dispatches and 258 logical outputs, along with guard elements.
Exact integer comparison is appropriate for this contract. It does not imply
bitwise CPU/GPU agreement for future floating-point transport.

Default macOS tests require a real Metal device. Hosted CI may configure
`RYPATH_ALLOW_NO_METAL_DEVICE=ON`; only an explicit absent default device can then
produce a visible CTest skip. Backend-disabled builds have their own stub tests.
Library, pipeline, allocation, command, and output failures never qualify as an
availability skip. Some sandboxes hide the device, so real hardware acceptance
must run in a process with GPU access.

The macOS job compiles host code and MSL and runs Apple Clang Static Analyzer on
the adapter. Linux retains compiled C++ CodeQL coverage. Neither successful
CodeQL analysis nor shader compilation alone establishes GPU correctness.
Before review, require a passing M4 Max run at the final revision, including
the API-validation run below, and record host/toolchain details and observed
results in [status](status.md):

```sh
MTL_DEBUG_LAYER=1 ctest --preset debug --no-tests=error --verbose -R '^metal\.'
python3 scripts/analyze_metal.py build/debug/compile_commands.json src/metal_backend.mm
```

The API-validation run selects the backend tests because Apple's debug layer
writes a startup banner to stderr. CLI output-contract tests run without that
environment flag. The deployment minimum is separate from the runtime OS
versions actually exercised.

## Primary references

- [Apple shader precompilation](https://developer.apple.com/documentation/metal/building-a-shader-library-by-precompiling-source-files)
- [Default device and CoreGraphics linkage](https://developer.apple.com/documentation/metal/mtlcreatesystemdefaultdevice%28%29)
- [Metal toolchain installation](https://developer.apple.com/documentation/Xcode/downloading-and-installing-additional-xcode-components)
- [Command completion](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilcompleted%28%29)
- [Metal API validation](https://developer.apple.com/documentation/xcode/validating-your-apps-metal-api-usage)
- [CodeQL language coverage](https://codeql.github.com/docs/codeql-overview/supported-languages-and-frameworks/)
