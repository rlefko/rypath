# PBRT preflight dependency decision

The first ingestion slice provides portable structural preflight, not rendering
translation. It uses a scoped adaptation of the tokenizer in
[PBRT v4 parser.cpp at b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/parser.cpp).
The pinned upstream scene builder and parameter implementations define the
interpretation being tested; the [format specification](https://www.pbrt.org/fileformat-v4)
provides the public contract.

The adaptation retains Apache-2.0 notices and the full upstream license.
Original rypath interpretation and reporting code remains GPL-3.0-only. The
Apache-2.0 contribution is distributed under its own terms within the GPLv3
project. See the [adaptation record](../third_party/pbrt/README.md).

## Integration boundary

The upstream tokenizer's whitespace, bracket, comment, quoted-string, and escape
scanning is small enough to review independently. The adaptation replaces
upstream process termination, mapped-file/global-statistics machinery, borrowed
token storage, and renderer-specific allocation with owned records, bounded
processing, and source-located diagnostics. Numeric conversion and typed parsing
are implemented locally. No upstream renderer library or build system is linked.

Importing the full parser would pull in scene builders, parameter dictionaries,
format/upgrade behavior, geometry, global options, and double-conversion. That
would obscure the narrower preflight contract. The chosen boundary allows
explicit error and dependency reporting without a third-party runtime package.

Alternatives reviewed during planning were ingowald/pbrt-parser
(`26571dae2a6dec05655116c9c3a5ac70421fee0b`, Apache-2.0, primarily v3),
mxpv/pbrt4 (`c4bb00e659978e6396b638cc1aa8b96e813a9cd3`, Apache-2.0,
Rust with incomplete Import/gzip handling), and mvictoras/photon
(`821adb5f5ee11f797fb1e6032026deeff0d3fa7e`, broader rendering dependencies
and no usable license notice found in the reviewed tree). None was integrated.

## Deliberate compatibility boundaries

The reader accepts signed decimal integers and finite decimal/scientific
binary64 values. It rejects hexadecimal values, nonfinite values, malformed
numeric suffixes, and range failures. Classic-locale stream conversion is used:
floating-point `std::from_chars` is unavailable at the macOS 14 deployment target
in the supported Apple standard library. Representable binary64 subnormals are accepted consistently across standard
libraries; underflow rounded to zero is reported. Parameter declarations must contain exactly a
type and a name. Legacy v3 directives receive explicit diagnostics.

Includes and input assets resolve from the supplied entry file's directory,
including nested includes. Canonical paths identify active include cycles, but
do not rewrite the supplied path used for resolution. Unlike upstream
`ResolveFilename`, an entry-relative miss does not fall back to the process's
working directory. Import and compressed scenes remain unvisited and visible.

The checked geometry domain is the existing float/index contract. Authored
binary64 values and numeric lexemes remain separate. Rounding is allowed when
it preserves that domain; overflow, collapsed triangles, and singular narrowed
transforms are errors. This is not a guarantee of world-space floating-point
intersection accuracy for arbitrarily ill-conditioned geometry.

## Corpus boundary

Routine tests use synthetic scenes and one unmodified 7,253-byte CC0 Cornell Box
fixture. Its [manifest](../tests/fixtures/pbrt/README.md) records
exact acquisition revision, hash, notice, expected diagnostics, and coverage.
It is a preflight fixture, not a rendering reference or a full PBRT corpus.
Nothing is downloaded by configure, build, or CTest.

An existing PBRT binary at the pinned revision may optionally compare `--format`
syntax acceptance. That mode does not establish scene-state or dependency
semantics. No matching binary was available locally, and the full renderer was
not downloaded or built for this slice.

## sRGB reconstruction extension

The RGB rendering slice adds a scoped adaptation of the same pinned PBRT
RGB-to-spectrum lookup and sigmoid model, based on
[Jakob and Hanika 2019](https://rgl.epfl.ch/publications/Jakob2019Spectral).
A canonical binary32 table and D65 data are fixed numerical renderer dependencies,
not downloaded scene assets. Their Apache-2.0 terms and generation provenance
are retained in the [adaptation record](../third_party/pbrt/README.md).

The 9 MiB binary is embedded as generated portable C++ byte chunks. This avoids
a 37.5 MB tracked decimal initializer, a runtime resource lookup, additional
platform assembly, or a toolchain-dependent optimization step during ordinary
builds. CMake verifies its checksum before embedding; the optional extraction
script requires an already-generated pinned reference table. Python and PBRT
remain unnecessary for routine product configuration and builds.

Only sRGB is included. Wider-gamut tables, RGB textures, and other spectral
representations require their own compatibility and numerical evidence.
