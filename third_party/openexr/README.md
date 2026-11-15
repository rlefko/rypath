# OpenEXRCore and bundled libdeflate

This is the static C11 subset of OpenEXR **3.5.4**, pinned to
[`86687e5f34e3a164820c5e280259550e3ca87c92`](https://github.com/AcademySoftwareFoundation/openexr/tree/86687e5f34e3a164820c5e280259550e3ca87c92).
The upstream archive SHA-256 is
`aba33fbd3660be758a25a6d797fced526f736383cead21be3ab56de434641ad2`.
The sources total approximately 1.7 MiB, including upstream's bundled
**libdeflate 1.26** in `external/deflate`. Upstream already adapts its internal
libdeflate symbols for embedding; its files are copied without further changes.

OpenEXR's BSD-3-Clause terms, contributors, and patent grant are retained in
`LICENSE.md`, `CONTRIBUTORS.md`, and `PATENTS`. libdeflate's MIT terms are retained
in `external/deflate/COPYING` and the source files. They retain their own terms
independently of the surrounding GPL-3.0-only project.

`UPSTREAM_SHA256SUMS` records the original upstream bytes for files copied into
this subset. `SHA256SUMS` records the shipped bytes, including the following
local adaptations. CMake checks the shipped hashes at configuration time.
Neither configuration nor compilation downloads data or discovers a host
OpenEXR, Imath, zlib, libdeflate, OpenJPH, or zstd package.

## Integration and adaptations

`cmake/OpenExrCore.cmake` owns an explicit source list and static C11 target.
The C++ OpenEXR/Imath layers, OpenJPH and zstd sources, upstream build scripts,
tools, and assets are excluded. The classic Core codecs NONE, RLE, ZIPS, ZIP,
PIZ, PXR24, B44, B44A, DWAA and DWAB remain compiled. B44/DWA use upstream's
fixed tables and thread-safe once initialization. The Core and libdeflate
allocations use the caller's bounded allocation callbacks.

On Linux, the target privately defines `_POSIX_C_SOURCE=200809L` and
`_DEFAULT_SOURCE` before including system headers. Strict C11 otherwise hides
Core's POSIX file APIs such as `O_CLOEXEC` and glibc's endian conversions and
`strcasecmp` declaration (the latter is included by `<string.h>` through
`<strings.h>` with the default feature set). This keeps C extensions disabled,
leaves the pinned upstream sources unchanged, and does not propagate feature
test macros into rypath's C++ consumers.

1. `src/lib/OpenEXRCore/openexr_config.h` replaces upstream's generated-config
   dependencies with a static-library export policy, enabled C/pthread once
   initialization, and `IMATH_VERSION_MAJOR=0`. This selects Core's existing
   non-Imath binary16 conversion fallback in `internal_coding.h`; it does not
   add an Imath library or lookup-table dependency.
2. The new `OpenEXRConfigInternal.h` enables upstream's embedded libdeflate
   amalgamation in `compression.c`, preserving its relative include layout.
3. `compression.c` dispatches HTJ2K, LJ2K and ZSTD operations to
   `EXR_ERR_FEATURE_NOT_IMPLEMENTED`. Their runtime implementations are
   excluded. These codecs have allocation paths outside the bounded classic
   Core allocation contract and are deferred. The rypath decoder also rejects
   them explicitly before creating a decoding pipeline.
4. `compression.c` checks the cached libdeflate decompressor allocation before
   invoking it. Upstream's cached path dereferenced null after an allocation
   callback failed; rypath's scratch-boundary test reproduced this under
   ASan/UBSan. The guard returns `EXR_ERR_OUT_OF_MEMORY` and leaves cleanup to
   the existing pipeline destructor.

The rypath wrapper supplies bounded input/size and positioned-output callbacks,
strict header validation, and disables chunk-table reconstruction. A scoped
thread-local allocation owner remains active through context destruction;
allocation headers retain their actual owner during nested calls. Separate
threads have separate scopes. Scratch limits include allocation-header overhead;
retained limits separately bound the decoded float RGB/alpha vectors. Decode
limits are capped at 4096 per data-window/tile dimension and 256 MiB per retained
or scratch budget before starting the Core context; caller limits can tighten
these caps. Output uses the caller's film dimensions and retained extent. Core
validates the display window's coordinates without constraining it to the data
window dimension limit; it remains metadata and allocates no expanded pixels.

## Supported image boundary

The wrapper reads single-part flat scanline or one-level tiled images, including
nonzero data-window origins and partial edge tiles. It retains the data window
without expanding to the display window. Only unit-sampled Y, RGB or RGBA with
one consistent HALF or FLOAT channel type are accepted. Y replicates to RGB;
HALF values widen exactly into binary32 while retaining HALF source metadata.
Nonfinite decoded samples are invalid. Missing chromaticities means linear
sRGB; authored sRGB chromaticities must satisfy PBRT's relative `<1e-3`
comparison. Other chromaticities, multipart, deep, mip/rip levels, UINT, mixed
sample types, partial/layered/extra channels and subsampling receive explicit
unsupported diagnostics. Malformed data and resource limits remain separate.

Output is RGB HALF or FLOAT with ZIP compression, linear-sRGB chromaticities and
caller-supplied string attributes. Nonfinite values and HALF overflow fail.
Reserved and duplicate attribute names fail. The caller owns publication and
positioned writes; successful encoding includes final offset-table writes and
context finalization. Negative and above-one finite values are retained.

Small independent fixtures and their generation recipe live in
`tests/fixtures/exr`; routine tests link only this static subset.
