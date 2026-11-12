# Dependencies and licensing

Original rypath code uses SPDX identifier `GPL-3.0-only`. The repository contains
the full unmodified GNU GPL v3 text in [LICENSE](../LICENSE).

The portable CLI has no third-party runtime dependencies beyond the C++ standard
library. The macOS adapters use the SDK's Metal, Foundation, CoreFoundation,
CoreGraphics, and ImageIO frameworks through Objective-C++ with ARC. The project
does not vendor or fetch metal-cpp.
Development tooling is separate from the distributed executable:

| Tool | Purpose | Version contract |
| --- | --- | --- |
| CMake | Configure/build/test presets | 3.25 minimum, CI uses 4.0.3 |
| Ninja | Build executor | Selected by all presets, CI uses 1.13.0 |
| Clang / Apple Clang | C++20 compilation | CI records the compiler version |
| Xcode / Metal compiler | Objective-C++ and offline MSL compilation | Hosted macOS uses Xcode 16.4; deployment minimum macOS 14, MSL 3.1 |
| clang-format | Source formatting | 18.1.8, shared by hooks and CI |
| clang-tidy | Focused static analysis | LLVM 18 |
| pre-commit | Local development hooks | Pinned in CI configuration |
| GitHub Actions / CodeQL | Hosted checks and compiled C++ analysis | Actions pinned by commit SHA |

The workflow and hook configuration are the executable source of truth for exact
tool pins. No PBRT, mesh-loader, metal-cpp, or denoising dependency is
vendored or fetched by the build. CodeQL does not analyze Objective-C++ or MSL;
the Apple adapter receives a separate native Clang analyzer pass, while shader
compilation and runtime validation cover the compute path.

The scalar spectral renderer embeds the official CIE 1931 two-degree observer
table. Its CSV is checksum-pinned, unmodified, and separately licensed CC BY-SA
4.0; the generated table transcription retains that license. Attribution,
metadata, and full terms are in [third_party/cie](../third_party/cie/README.md).
No dataset is fetched during configuration or compilation.

The small CPU/MSL Philox4x32-10 implementations and known-answer vectors follow
Random123 v1.14.0. Its BSD license and source record are retained in
[third_party/random123](../third_party/random123/README.md). No Random123 library
or third-party runtime package is linked. Original demo geometry and material
spectra are project assets. These separate terms are not replaced by GPL headers
on surrounding project code.

Future dependency additions must record their upstream revision, license, required
notices, integration boundary, and supported functionality. Scene assets retain
individual terms and credits; project licensing does not relicense them.
