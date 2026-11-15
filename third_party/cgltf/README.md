# cgltf structural decoder

Upstream: https://github.com/jkuhlmann/cgltf

Pinned revision: `85cd62382dfea638278962690cf515023f33ed00` (2026-02-02).
The upstream header reports version 1.15; this is newer than the v1.15 tag.
MIT terms are retained unmodified in LICENSE and the header.

Shipped files are unmodified:

| File | SHA-256 |
| --- | --- |
| cgltf.h | efb169dee911696b5d35fc8e3f7ea0c56d679debc529eba9ca6aa6443ba9d5e9 |
| LICENSE | f619925f80ef862497aaf8e8155ef218fa6a2190055129523ca3df9119a9ba95 |

`src/gltf.cpp` statically includes the implementation, using memory-only parsing,
accounted allocation callbacks and locale-independent checked numeric hooks.
The adapter first checks strict JSON, duplicate keys, depth/tokens, required core
structure and references. Decoded keys/values are replayed as bounded canonical JSON
before cgltf parsing; escaped keys and Unicode surrogate-pair names retain their
meaning. Original immutable source bytes and locations remain provenance.
It owns URI/resource snapshots, framing, selected scene
semantics and unsupported-feature diagnostics. No library file/image I/O is used.
`cgltf_validate()` is not the safety boundary: its range expressions contain
unchecked arithmetic. Accessor data is decoded by the checked project adapter;
upstream sparse helpers step replacement values by the base accessor stride,
which is unsuitable for tightly packed sparse values over interleaved base data.
No decoder helper reads unvalidated buffer ranges. No build/runtime network fetch.

The shared `Limits.work_items` default is 2^26. Accessor decode, sparse-index
validation, placed-triangle narrowing checks and tangent generation consume it
before their loops. Mikk generation conservatively reserves faces squared for
welding/grouping, so the default permits fewer than 8,192 normal-mapped triangles
without authored tangents after other work. Callers may choose a larger explicit
budget. Overflow and work exhaustion fail visibly; no partial geometry is emitted.
Source-location lookup uses a bounded newline index rather than rescanning prefixes.
