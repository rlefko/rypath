# Dependencies and licensing

Original rypath code uses SPDX identifier `GPL-3.0-only`. The repository contains
the full unmodified GNU GPL v3 text in [LICENSE](../LICENSE).

The foundation executable has no third-party runtime dependencies beyond the
C++ standard library. Development tooling is separate from the distributed
executable:

| Tool | Purpose | Version contract |
| --- | --- | --- |
| CMake | Configure/build/test presets | 3.25 minimum, CI uses 4.0.3 |
| Ninja | Build executor | Selected by all presets, CI uses 1.13.0 |
| Clang / Apple Clang | C++20 compilation | CI records the compiler version |
| clang-format | Source formatting | 18.1.8, shared by hooks and CI |
| clang-tidy | Focused static analysis | LLVM 18 |
| pre-commit | Local development hooks | Pinned in CI configuration |
| GitHub Actions / CodeQL | Hosted checks and compiled analysis | Actions pinned by commit SHA |

The workflow and hook configuration are the executable source of truth for exact
tool pins. No PBRT, mesh-loader, metal-cpp, image-output, or denoising dependency is
vendored or fetched by the foundation build.

Future dependency additions must record their upstream revision, license, required
notices, integration boundary, and supported functionality. Scene assets retain
individual terms and credits; project licensing does not relicense them.
