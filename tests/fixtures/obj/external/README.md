# Small external OBJ/MTL fixture

Unmodified TinyObjLoader `models/cube.obj` and `models/cube.mtl`, revision
`a6a5714eb144158373899faf2b899811a36d9fba`:
https://github.com/tinyobjloader/tinyobjloader/tree/a6a5714eb144158373899faf2b899811a36d9fba/models

The upstream repository distributes these files under its MIT license, with no
asset-specific alternate notice observed. Its original combined `LICENSE` is
retained here without relicensing the files. Acquisition closure is two text
files, 545 + 204 = 749 bytes; no images are referenced. `SHA256SUMS` records exact
unmodified bytes. Routine tests read the checked-in copies without network access.

Selected coverage: eight positions, six quad faces, twelve output triangles,
multiple group names, material changes, material persistence across a group
change and five raw material definitions. The unused material named `light`
contains `Ka 20 20 20`; that legacy ambient coefficient is not an emission claim.
No upstream scene/camera/render equivalence is claimed. Synthetic test strings in
`tests/obj_tests.cpp` are separately project-authored GPL-3.0-only fixtures and
cover indices, concavity, collinearity, smoothing, malformed inputs and bounds.
