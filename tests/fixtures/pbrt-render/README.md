# PBRT spectral render fixture manifest

The nine rypath-authored entry points are licensed under GPL-3.0-only.
`cornell-box-adapted.pbrt` is a CC0 derivative of the [pinned Cornell Box
fixture](../pbrt/README.md) by Benedikt Bitterli, converted to PBRT v4 by Matt
Pharr. The [upstream notice](../pbrt/LICENSE.txt) applies to the Cornell scene.
All geometry and spectral values are inline. Each entry point expands to its
listed size with zero external dependencies (13,345 bytes total).

| Entry point | Bytes | SHA-256 | Geometry |
| --- | ---: | --- | --- |
| `background.pbrt` | 651 | `95612a748fa17e35844bd5ccd86c22d991281885fea2ab6a68675cc09583602b` | 1 mesh, 3 vertices, 1 triangle |
| `diffuse.pbrt` | 654 | `4a342f8759f744112076f72ef091cbbc90811040f0dd745c7f282ac129cceb71` | 1 mesh, 4 vertices, 2 triangles |
| `multibounce.pbrt` | 1,434 | `6a9df8ae504134824986df5bafe1e24d84939d0da4f17042f45a8b1495711cb3` | 5 meshes, 20 vertices, 10 triangles |
| `rgb-background.pbrt` | 657 | `0f550eebefcad23b0c3baa044739509a53772f9512604cd4fd527403d338d8d0` | 1 mesh, 3 vertices, 1 triangle |
| `rgb-diffuse.pbrt` | 614 | `ccce5d120a3f8625453873973cffb28ca7c158214259deb666f16b039a8c9f57` | 1 mesh, 4 vertices, 2 triangles |
| `rgb-multibounce.pbrt` | 1,380 | `afe3043c925b55a9bf37a0e637b743fcc861b7e52359f0448bfed584fd4e7754` | 5 meshes, 20 vertices, 10 triangles |
| `area-visible.pbrt` | 983 | `34d08504ae1eb4b19b960bfb19a5c2614effb0062c932e701211ff3ff50c4389` | 2 meshes, 8 vertices, 4 emissive triangles |
| `area-direct.pbrt` | 953 | `d9c22f5e038963e3491116b6685b6ed01c6126257cc517a277730ae4011ac94e` | 2 meshes, 7 vertices, 3 triangles, 1 emissive |
| `area-mixed.pbrt` | 1,726 | `a4cd3482f208a49e56e2672c238adedddd0c18523f12a0a54937b53a4820c176` | 6 meshes, 23 vertices, 11 triangles, 3 emissive |
| `cornell-box-adapted.pbrt` | 4,293 | `652e63dc4806c5d3a33e6560b654eaf775fa80ef9e1df8243ebe1a9494698aa6` | 8 meshes, 72 vertices, 36 triangles, 2 emissive |

Each scene uses a pinhole perspective camera, an 8×8 full-frame sRGB film
with the CIE 1931 sensor, ISO 100, the default shutter interval
[0, 1], `savefp16=false`, a box filter of radius 0.5 on both axes, an
independent sampler, and the path integrator. Cornell preserves its authored
camera pose. The six background, diffuse, and multibounce fixtures each use
one uniform infinite light. Their RGB variants exercise explicit RGB or omitted
D65 emission.

| Entry point | FOV | Path maxdepth | Spectral and geometric coverage |
| --- | ---: | ---: | --- |
| `background.pbrt` | 50° | 0 | A triangle behind the camera leaves the primary rays unobstructed. The light spectrum peaks at 460 nm and uses scale 2 to exercise negative and above-one linear RGB. |
| `diffuse.pbrt` | 50° | 1 | An unobstructed quad covers the frame. The broad light spectrum illuminates a diffuse reflectance with a knot at 503.5 nm. |
| `multibounce.pbrt` | 58° | 5 (default) | An asymmetric opening, five quads and four diffuse spectra exercise visibility and successive surface scattering. |
| `rgb-background.pbrt` | 50° | 0 | HDR RGB [4, 0.15, 1.2], scale 1.7 and illuminance 7 distinguish D65 normalization from tinted-spectrum normalization. |
| `rgb-diffuse.pbrt` | 50° | 1 | Chromatic RGB [0.08, 0.7, 0.31] on a covering quad under omitted-L D65 illumination. |
| `rgb-multibounce.pbrt` | 58° | 2 | The asymmetric five-quad scene mixes original sampled materials and RGB side walls under chromatic RGB illumination. |
| `area-visible.pbrt` | 45° | 0 | A one-sided RGB emitter faces the camera and a two-sided sampled-spectrum emitter faces away. |
| `area-direct.pbrt` | 7.15266875° | 1 | A finite off-axis triangle illuminates a diffuse receiver under a sampled spectrum. |
| `area-mixed.pbrt` | 58° | 5 | Three unequal-area emitting triangles use RGB or omitted D65 emission alongside an RGB environment in an asymmetric room. |
| `cornell-box-adapted.pbrt` | 19.5° | 65 | Two downward-facing triangles emit from the ceiling fixture above two boxes and colored walls. |

The Cornell derivative adds an attribution header, replaces Sobol with
independent sampling (seed 0, 4,096 spp), replaces the radius-1 triangle filter
with the radius-0.5 box filter, changes 1,024×1,024 to 8×8, sets the CIE 1931
sensor and ISO 100 explicitly, and sets `savefp16=false`. It removes eight
authored normal arrays and eight UV arrays. Geometry, winding, materials,
RGB emission, scoped state, camera and transform, and the path depth of 65
retain their original authored values. The removed normals agree with the
geometric normal orientations and differ slightly in direction. The derivative
retains `cornell-box.png` as its authored film filename. The reference script
uses PBRT's `--outfile` and the rypath harness's positional output argument
to write PFM. The product CLI uses `--output` to select the PNG path.

The reference is the CPU Release/float build of [PBRT v4 at
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
Both renderers use 4,096 samples per pixel and 16 fixed replicates per scene,
with rypath seeds 0–15 and PBRT seeds 16–31. The comparison script changes the
authored seed for each replicate and uses PBRT's output override for linear
binary32 PFM. The rypath harness writes its production film projection in the
same format.

The comparison uses the full 8×8 frame for every scene and the four top-down
4×4 quadrants for both multibounce scenes, the three area scenes, and the
adapted Cornell scene. Each region's R, G, and B means must
agree within five combined standard errors plus
`2e-5 * max(1, abs(reference))`. See the [render profile](../../../docs/pbrt-rendering.md)
for the comparison command.
