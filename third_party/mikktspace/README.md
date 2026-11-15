# MikkTSpace

Upstream: https://github.com/mmikk/MikkTSpace

Pinned revision: `3e895b49d05ea07e4c2133156cfa94369e19e409` (2020-03-25),
header version 1.0. The zlib-style copyright/permission notices in both source
files remain complete and unmodified. Original code: Morten S. Mikkelsen, 2011.

| File | SHA-256 |
| --- | --- |
| mikktspace.c | de87e74107df766ce68108801262bd8d53899414236b59810509a8fc2a51e288 |
| mikktspace.h | 17fc433894f24c73753d548086cc4d8c5c0379f4a6edfb98b5da243e4f0bc3d0 |

The source files are unmodified. `src/gltf.cpp` includes the C implementation in
its decoder translation unit, adapting malloc/free through a bounded per-load
allocator using preprocessor substitution. No global allocator replacement is
installed. Overflow-safe input counts precede signed-int algorithm entry. Any
allocation-budget exhaustion fails conversion even if an upstream fallback
would return success. Per-corner callback results remain split and preserve the
bitangent sign; existing indices never overwrite or average tangent frames.
Generation uses glTF positions, normals and the selected normal-map UV set without
coordinate inversion. No build/runtime download occurs.
