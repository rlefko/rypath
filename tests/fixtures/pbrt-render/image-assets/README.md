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
The GPL-3.0-only wrapper `../image-trilinear.pbrt` places the same image on
a front-facing pair of triangles. The near-center footprint selects a
fractional LOD around 2.5, blending the 8×8 and 4×4 mips. A triangle pixel
filter with radii (0.0625, 0.0625) keeps the blend region near the red
transition at repeat coordinates (0.025, 0.5).

[The dependency manifest](../dependencies.json) records the size and SHA-256
of each input staged with its entry point. All five image texture fixtures use
8×8 linear RGB output and 4,096 samples per pixel. Comparison runs use
independent seeds per backend and leave PBRT texture filtering enabled.
The trilinear run compares the full image and the two pixels in column 4,
rows 3–4 (zero-indexed, top-down). It also changes just the texture filter to
bilinear at the first seed for each backend (rypath 0, PBRT 16) and requires
the bilinear red mean to exceed the trilinear red mean by more than 0.02 in
those two pixels.
