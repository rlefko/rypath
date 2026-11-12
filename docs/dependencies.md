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
tool pins. No PBRT renderer, mesh-loader, metal-cpp, or denoising runtime dependency is
linked or fetched by the build. CodeQL does not analyze Objective-C++ or MSL;
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

The preflight reader contains a scoped adaptation of PBRT v4’s Apache-2.0
tokenizer at `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`. Its notices,
license, and adaptation boundary are recorded in
[third_party/pbrt](../third_party/pbrt/README.md) and the
[decision record](pbrt-decision.md). The small unmodified CC0 Cornell Box fixture
retains its own notice, checksum, revision, and coverage manifest in
[tests/fixtures/pbrt](../tests/fixtures/pbrt/README.md).

The optional numerical reference builds that same pinned PBRT renderer and its
exact submodules in an ignored local cache. It retains upstream Apache-2.0 and
dependency notices there. Its renderer and dependency libraries are not linked
into rypath. One canonical 9,437,440-byte sRGB reconstruction table and the pinned
D65 spectrum are vendored separately under the PBRT attribution record. CMake
verifies the binary hash and embeds portable byte chunks without running the
optimizer or downloading data. The lookup/evaluation adaptation retains upstream
Apache-2.0 notices. Other translation and Metal code is original project code.
New comparison scenes are project-authored GPL-3.0-only fixtures. Reference
provenance and build instructions are recorded in
[PBRT render validation](pbrt-render-validation.md).
