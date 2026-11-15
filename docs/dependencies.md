# Dependencies and licensing

Original rypath code uses SPDX identifier `GPL-3.0-only`. The repository contains
the full unmodified GNU GPL v3 text in [LICENSE](../LICENSE).

The portable CLI builds its vendored PNG, JPEG, and OpenEXR decoders into the executable and
requires no external third-party libraries at runtime. Its OpenEXRCore and
embedded libdeflate subset also uses C11 and platform threading support. The macOS adapters use
the SDK's Metal, Foundation, CoreFoundation,
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
optimizer or downloading data. The lookup/evaluation, image mip, and footprint
adaptations retain upstream Apache-2.0 notices.
Owned comparison scenes are GPL-3.0-only. The separately named Cornell rendering
adaptation retains CC0 attribution and the unchanged original notice; its
[render fixture manifest](../tests/fixtures/pbrt-render/README.md) records every
adaptation and both source/derivative hashes. Reference
provenance and build instructions are recorded in
[PBRT render validation](pbrt-render-validation.md).

The [Sobol data and routines](../third_party/pbrt/sobol.md) use the same pinned
PBRT revision. The 1,024-dimensional direction table and interval matrices are
checksum-verified at configure time and shared by portable and Metal sampling.
Their PBRT Apache-2.0 and Leonhard Gruenschloss permission notices are retained.
No sampler library is linked or downloaded at runtime.

The bounded spectrum file decoder is original C++20 code using the
existing locale-independent numeric policy. Its syntax and curve support were
checked against pinned PBRT's `ReadFloatFile` and `PiecewiseLinearSpectrum::Read`.
The independent validation probe links the cached PBRT reference only; the product
adds no runtime dependency. Spectrum fixtures are original project assets.

All 88 built-in named spectra from the same pinned PBRT revision are embedded
as immutable binary32 samples over 360–830 nm for lights and original prepared
knots for diffuse reflectance. The reproducible extraction
preserves upstream interpolation, endpoint extension, and illuminant normalization.
CMake verifies both generated data checksums. The [PBRT record](../third_party/pbrt/README.md)
retains extraction/oracle commands, PBRT and PhysLight notices, and the glass data's
refractiveindex.info attribution. These tables add no runtime or build-time network dependency.

The bounded PLY 1.0 decoder is original C++20 code. The reader APIs and count
handling in pinned PBRT RPly 1.1.3, tinyply 3.0, and happly were examined before
choosing this boundary; enforcing allocation and decode-work bounds would require
changes inside those readers, not just an adapter around their file/stream APIs.
No new runtime dependency is linked. The bilinear intersection mathematics is
adapted from pinned PBRT with explicit Apache-2.0 attribution and compensated
arithmetic for Metal; its independent CPU oracle is test-only. See the
[PBRT adaptation record](../third_party/pbrt/README.md).

The 412-byte external PLY comparison asset retains its CC BY 2.0 terms and
upstream attribution in its [manifest](../tests/fixtures/pbrt-render/ply-assets/README.md).
Owned wrappers and synthetic mesh fixtures have separate GPL-3.0-only terms.

Portable PNG input uses the zlib-licensed LodePNG decoder at revision
`7b2893e35891c0c85bee5595caafaf3c2044b409`. Its in-memory decode API and
configurable allocation and inflate limits let the preflight reader own file
I/O and enforce the same profile on Linux and macOS. The static build disables
the encoder, disk I/O, C++ wrapper, and ancillary metadata decoding. A local
extension caps work at 65,536 DEFLATE blocks per stream. The reader also checks
image dimensions, decompressed scanline size, scratch memory, and aggregate
retained image payloads. Static noninterlaced and Adam7 input supports grayscale
at 1/2/4/8/16 bits, grayscale with alpha/RGB/RGBA at 8/16 bits, and indexed
color at 1/2/4/8 bits. Packed grayscale expands to RGB8 using the decoder.
The adapter validates packed palette indices before RGB8 expansion. RGBA alpha
is retained for scalar filter branches; alpha never composites RGB reflectance,
and color metadata is ignored. The [LodePNG record](../third_party/lodepng/README.md)
retains the source pin, hashes, modifications, and license.

The unmodified PNGSuite comparison images retain their original notice in the
[image fixture manifest](../tests/fixtures/pbrt-render/image-assets/README.md).

JPEG input uses the same `stb_image` revision `af1a5bc352164740c1cc1354942b1c6b72eacb8a`
as the pinned PBRT reference, under its retained dual MIT/public-domain terms.
Only in-memory JPEG decoding is compiled. The wrapper owns bounded allocations,
validates marker framing and color modes, and limits scans and DCT work. Local
hardening rejects incomplete component/scan/restart data and widens vulnerable
IDCT arithmetic. Source hashes, patch boundaries, and license are in the
[stb record](../third_party/stb/README.md). It adds no runtime or build-time network
dependency. Owned JPEG fixtures record their source recipe and hashes separately.


Portable EXR input and output statically compile OpenEXRCore 3.5.4 at
`86687e5f34e3a164820c5e280259550e3ca87c92`, including upstream's embedded
libdeflate 1.26. OpenEXR's BSD-3-Clause terms, contributors and patent grant,
and libdeflate's MIT license remain in [the dependency record](../third_party/openexr/README.md).
CMake verifies shipped file hashes and builds an explicit C11 source subset;
it does not discover or download installed OpenEXR, Imath, zlib, OpenJPH or zstd.
The C++ SDK and newer compression dependencies are excluded. The wrapper owns
bounded memory, channel/color validation and positioned I/O. The local adaptation
record identifies disabled codecs and the checked decompressor-allocation fix.
The supported input subset is narrower than the compiled classic codecs.

Radiance RGBE decoding is original bounded C++20 code following the
[official Radiance format](https://radsite.lbl.gov/radiance/refer/filefmts.pdf)
and the pinned PBRT/stb numeric conversion. It adds no library dependency.

HDR validation separately compiles an installed OpenEXR C++ reader/generator
and a pinned PBRT probe. These oracle libraries are never product dependencies.
The selected external OpenEXR `comp_zip.exr` asset is pinned to upstream
`f723c3940fff287c0a26b425b90a8e764823bfd4` and remains cache-only. Its BSD license,
size, hash, upstream path and project-authored wrapper boundary are recorded in
the [HDR fixture manifest](../tests/fixtures/pbrt-render/hdr-io/README.md).
The small independently generated HALF, FLOAT and RGBE images are owned fixtures.
