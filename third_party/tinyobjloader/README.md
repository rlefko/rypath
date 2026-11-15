# TinyObjLoader C11 adapter

Upstream: https://github.com/tinyobjloader/tinyobjloader
Revision: `a6a5714eb144158373899faf2b899811a36d9fba` (2026-11-15).
The shipped source subset retains upstream MIT terms in `LICENSE`. The license
also records upstream's optional Earcut ISC notice; our separately pinned Earcut
copy has its own record. No upstream example assets are included here.

The C11 `tiny_obj_c` callback API is compiled in scalar, single-threaded,
memory-only mode. No file-I/O, mmap or SIMD/thread extension is enabled. Its
`tobj_tess` dependency is compiled for linkage but **never used to triangulate
imported geometry**. The rypath wrapper owns all reads and source snapshots.

The sole source modification is one include of local `rypath_numeric.h` in
`tiny_obj_c.c`. It selects upstream's existing `TOBJ_SCAN_DOUBLE_OVERRIDE` hook,
implemented by `rypath_obj_scan_double` in `src/obj.cpp`, for the project's strict,
locale-independent binary64 decimal parser. No nan/inf substitution, truncated
mantissa conversion or locale-dependent fallback is permitted. The local header
is GPL-3.0-only; upstream source retains MIT terms. `SHA256SUMS` records shipped
bytes, including the adapted C file and local header.

This is a deliberately narrow callback integration, not a claim that upstream's
full scene parser has been hardened. A strict provenance pass first validates
OBJ record cardinalities and complete decimal/index tokens, resolves each
optional index against its own current array, retains original polygon corners
and metadata, and snapshots/raw-tokenizes MTL properties. It feeds only validated
`v/vt/vn/f` records to the callback decoder. The C11 decoder supplies scalar
arrays and must agree with every strict-pass corner index, attribute count and
face arity. Authored `o/g/s`, forward material bindings and raw MTL field presence
remain wrapper-owned. MTL semantic conversion is a separate project stage;
upstream's material parsing/defaults, unknown-directive skipping, smoothing/group
conflation and best-effort triangulation paths are not used.

Lexing accepts ASCII whitespace, CR/LF/CRLF, optional leading UTF-8 BOM,
backslash-newline continuations, `#` comments, quoted filename/name tokens and
escaped whitespace, quotes, hash and backslash. The latter quoted/escaped name
forms are declared extensions. Other backslashes are retained. NUL/control bytes,
empty quoted tokens, incomplete continuations and unterminated quotes fail.
Nonunit vertex weights and nonzero texture depth are unsupported. Numeric vertex
colors, points, lines, free-form records, `call` and `csh` are unsupported; no
scene-provided commands execute and no inclusion is followed.

Both passes share source/token/value/directive limits. Conservative cumulative
working accounting includes snapshots, strings, owned vector capacities,
metadata copies, geometry, C11 allocation requests and reserved Earcut workspace;
freed requests are not refunded. Retained output is separately capped. Geometry
has per-face corner and aggregate quadratic work limits; material binding uses
ordered lookup. C11 allocation callbacks return allocation failure when their
remaining budget is exhausted. The scalar/corner callback never lets a C++
exception cross the C ABI. No build-time or runtime network fetch occurs.
