# Selected external STL fixture

`triangle.stl` is unmodified from the Assimp project's
[`test/models/STL/triangle.stl`](https://github.com/assimp/assimp/blob/6bf4b3a4f0d1750c4a4cdc5a3dd0fef4ec0a666d/test/models/STL/triangle.stl),
at revision `6bf4b3a4f0d1750c4a4cdc5a3dd0fef4ec0a666d`.
The complete asset closure is one 176-byte ASCII STL with no dependencies.
Acquisition and expanded size are both 176 bytes; the retained upstream license
adds 3,791 bytes. `SHA256SUMS` pins both files and CMake checks them offline.

The Assimp team's modified BSD-3-Clause terms are retained in `LICENSE`.
The pinned repository Readme identifies `test/models` as BSD-licensed models;
the license's model exception applies to the separate `test/models-nonbsd`
directory. No STL-specific notice overrides those terms. The file has no
individual artist attribution; none is inferred. Project licensing does not
replace these asset terms.

The asset has one named solid, one facet with vertices `(1,1,0)`, `(-1,1,0)`,
`(0,-1,0)`, a +Z authored normal, and an unnamed `endsolid`. Its area is exactly
2. It establishes selected external ASCII acceptance, not closed-solid,
vendor-color, or broad corpus coverage. Owned binary and color fixtures are
generated independently in the test suites.

An owned PBRT wrapper at the converted bundle root supplies the camera,
lighting, film and sampling settings documented in
[STL validation](../../../../docs/stl-validation.md). The wrapper does not alter
the source STL or establish support for an upstream-authored complete scene.
