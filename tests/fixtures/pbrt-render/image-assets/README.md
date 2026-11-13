# Image texture fixtures

`asymmetric.png` and `high-frequency.png` are rypath-authored RGB8 fixtures under
GPL-3.0-only. The first is a 3×5 image with independently varying rows and
columns. The second is a 16×16 image with high-frequency variation and a
horizontal change in average color.

`z03n2c08.png` is an unmodified 32×32 RGB8 PNG by Willem van Schaik from
[PNGSuite in the stb repository](https://github.com/nothings/stb/blob/af1a5bc352164740c1cc1354942b1c6b72eacb8a/tests/pngsuite/primary/z03n2c08.png),
at the stb revision pinned by
[PBRT v4 b0bdf461](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/ext).
The original terms are in [PngSuite.LICENSE](PngSuite.LICENSE).
The GPL-3.0-only wrapper `../image-ply.pbrt` uses this image on the project-owned
curved quad and triangle in `../ply-assets/mixed.ply`.

[The dependency manifest](../dependencies.json) records the size and SHA-256
of each input staged with its entry point. All four image texture fixtures use
8×8 linear RGB output and 4,096 samples per pixel. Comparison runs use
independent seeds per backend and leave PBRT texture filtering enabled.
