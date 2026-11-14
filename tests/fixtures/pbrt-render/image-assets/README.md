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

The GPL-3.0-only wrapper `../image-point.pbrt` uses the same PNG and
triangle geometry with a UV mapping that selects the 8×8 mip at LOD 2.625.
The triangle pixel filter has radii (1/256, 1/256), keeping sample positions
within their selected mip texels. Column 4 stays near the repeat coordinate
`s = 0.025`, where point sampling selects the bright red texel and bilinear
sampling mixes in roughly 30% of the dark texel across the repeat seam.

The GPL-3.0-only wrapper `../image-ewa.pbrt` uses the same PNG and triangle
geometry with linear encoding, repeat wrap, EWA filtering, and an explicit
anisotropy cap of 8. Its radius-1/256 triangle pixel filter keeps pixel (4, 3)
near flipped UV (0.025, 0.6). The approximate flipped UV differentials
(0.075, −0.370) and (0.018, 0.008) give a rotated ellipse with a derivative
length ratio around 19. The cap of 8 widens the shorter axis, giving a
fractional EWA LOD near 0.6 that blends the 32×32 and 16×16 mips. This owned
wrapper provides coverage of the pinned asset.

[The dependency manifest](../dependencies.json) records the size and SHA-256
of each input staged with its entry point. All seven image texture fixtures use
8×8 linear RGB output and 4,096 samples per pixel. Comparison runs use
independent seeds per backend and leave PBRT texture filtering enabled.
The trilinear run compares the full image and the two pixels in column 4,
rows 3–4 (zero-indexed, top-down). It also changes just the texture filter to
bilinear at the first seed for each backend (rypath 0, PBRT 16) and requires
the bilinear red mean to exceed the trilinear red mean by more than 0.02 in
those two pixels.
The point run uses the same two regions and control seeds. It requires the
point red mean to exceed the bilinear red mean by more than 0.02 in those
two pixels.
The EWA run compares the full image and pixel (4, 3) using the same fixed
replicates and statistical gates. At the first seed for each backend, the EWA
red value at that pixel must exceed the trilinear control and the EWA cap-4
control by more than 0.02 each. Each control changes only its named parameter.
