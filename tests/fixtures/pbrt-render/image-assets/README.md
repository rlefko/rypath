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

`asymmetric16.png` is a rypath-authored 4×2 RGB16 image under GPL-3.0-only.
Each channel has a constant high byte (`01`, `03`, `0b` for R, G, B), and the
two rows vary each channel's low byte independently. The comparison script
defines the source samples in top-down order and regenerates the PNG for a
byte comparison with the checked fixture. [PNG16 comparison script](../../../../scripts/compare_pbrt_image16.py)

`basn2c16.png` (32×32 RGB16) and `basn0g16.png` (32×32 grayscale16) are
unmodified PNGSuite images by Willem van Schaik at the same pinned stb revision.
Their original terms are retained. [RGB16 source](https://github.com/nothings/stb/blob/af1a5bc352164740c1cc1354942b1c6b72eacb8a/tests/pngsuite/16bit/basn2c16.png),
[grayscale16 source](https://github.com/nothings/stb/blob/af1a5bc352164740c1cc1354942b1c6b72eacb8a/tests/pngsuite/16bit/basn0g16.png),
[PngSuite.LICENSE](PngSuite.LICENSE)

The GPL-3.0-only PNG16 wrappers use the following mappings and comparison regions.

| Entry point | Image and mip footprint | Comparison regions |
| --- | --- | --- |
| `image16-magnified.pbrt` | Left and right cards point-sample all eight texels with clamp wrap. The left uses default sRGB and scales the linear values by 128. The right uses linear encoding and scale 16. | Full frame and four quadrants |
| `image16-trilinear.pbrt` | `basn2c16.png` with default sRGB and repeat wrap. The center footprint blends the 8×8 and 4×4 mips. | Full frame and column 4, rows 3–4 |
| `image16-ewa.pbrt` | `basn0g16.png` in linear encoding with repeat wrap. The rotated footprint at pixel (4, 3) uses the anisotropy cap of 8 and blends the 32×32 and 16×16 mips near the repeat seam. | Full frame and pixel (4, 3) |

All three use a triangle pixel filter. Radii are 1/16 for trilinear and 1/256
for the magnified and EWA wrappers. Region coordinates are zero-based and top-down.
The numerical mip comparison checks both encodings for all three PNG16 assets.

`s35n3p04.png` (35×35, indexed4) and `tp1n3p08.png` (32×32, indexed8 with
`tRNS`) are unmodified PNGSuite images by Willem van Schaik at the same pinned
stb revision. Their original terms apply.
[Indexed4 source](https://github.com/nothings/stb/blob/af1a5bc352164740c1cc1354942b1c6b72eacb8a/tests/pngsuite/primary/s35n3p04.png),
[indexed8 source](https://github.com/nothings/stb/blob/af1a5bc352164740c1cc1354942b1c6b72eacb8a/tests/pngsuite/primary/tp1n3p08.png),
[PngSuite.LICENSE](PngSuite.LICENSE)

The GPL-3.0-only wrapper `../image-indexed-point.pbrt` uses default sRGB
encoding and clamp wrap. A radius-1/256 triangle filter keeps the top-left
film pixel at the center of base-level texel (2, 2), whose palette entry
has RGB (255, 255, 255) and alpha 0. The single-pixel comparison region
detects any loss of that RGB through alpha compositing.
The GPL-3.0-only wrapper `../image-indexed-trilinear.pbrt` uses linear encoding
and repeat wrap. It resizes the 35×35 image to 64×64 and minifies it at LOD
approximately 3.49, blending the 8×8 and 4×4 mips. Its triangle filter uses
radii of 1/16, and its blend region covers column 4, rows 3–4.

`invalid-index.png` and `over-limit-indexed.png` are rypath-authored CLI
preflight fixtures under GPL-3.0-only. The first stores index 1 in a 1×1
image with one palette entry. The second stores index 0 throughout a
valid 4097×1 indexed image that exceeds the texture axis limit.

[The dependency manifest](../dependencies.json) records the size and SHA-256
of each input staged with its entry point. All twelve image texture comparisons use
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
