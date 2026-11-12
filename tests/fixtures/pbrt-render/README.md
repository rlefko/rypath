# PBRT spectral render fixture manifest

These six entry points were authored for rypath and are licensed under
GPL-3.0-only. All geometry and spectral values are inline. The complete input
dependency closure is the six scene files below (5,390 bytes total, zero
external dependencies). Each entry point expands to its listed size.

| Entry point | Bytes | SHA-256 | Geometry |
| --- | ---: | --- | --- |
| `background.pbrt` | 651 | `95612a748fa17e35844bd5ccd86c22d991281885fea2ab6a68675cc09583602b` | 1 mesh, 3 vertices, 1 triangle |
| `diffuse.pbrt` | 654 | `4a342f8759f744112076f72ef091cbbc90811040f0dd745c7f282ac129cceb71` | 1 mesh, 4 vertices, 2 triangles |
| `multibounce.pbrt` | 1,434 | `6a9df8ae504134824986df5bafe1e24d84939d0da4f17042f45a8b1495711cb3` | 5 meshes, 20 vertices, 10 triangles |
| `rgb-background.pbrt` | 657 | `0f550eebefcad23b0c3baa044739509a53772f9512604cd4fd527403d338d8d0` | 1 mesh, 3 vertices, 1 triangle |
| `rgb-diffuse.pbrt` | 614 | `ccce5d120a3f8625453873973cffb28ca7c158214259deb666f16b039a8c9f57` | 1 mesh, 4 vertices, 2 triangles |
| `rgb-multibounce.pbrt` | 1,380 | `afe3043c925b55a9bf37a0e637b743fcc861b7e52359f0448bfed584fd4e7754` | 5 meshes, 20 vertices, 10 triangles |

Each scene uses an identity pinhole perspective camera, an 8×8 full-frame
sRGB film with the CIE 1931 sensor, ISO 100, the default shutter interval
[0, 1], `savefp16=false`, a box filter of radius 0.5 on both axes, an
independent sampler, and the path integrator. All six use one spatially uniform infinite light; the RGB variants exercise
explicit RGB or omitted D65 emission.

| Entry point | FOV | Path maxdepth | Spectral and geometric coverage |
| --- | ---: | ---: | --- |
| `background.pbrt` | 50° | 0 | A triangle behind the camera leaves the primary rays unobstructed. The light spectrum peaks at 460 nm and uses scale 2 to exercise negative and above-one linear RGB. |
| `diffuse.pbrt` | 50° | 1 | An unobstructed quad covers the frame. The broad light spectrum illuminates a diffuse reflectance with a knot at 503.5 nm. |
| `multibounce.pbrt` | 58° | 5 (default) | An asymmetric opening, five quads and four diffuse spectra exercise visibility and successive surface scattering. |
| `rgb-background.pbrt` | 50° | 0 | HDR RGB [4, 0.15, 1.2], scale 1.7 and illuminance 7 distinguish D65 normalization from tinted-spectrum normalization. |
| `rgb-diffuse.pbrt` | 50° | 1 | Chromatic RGB [0.08, 0.7, 0.31] on a covering quad under omitted-L D65 illumination. |
| `rgb-multibounce.pbrt` | 58° | 2 | The asymmetric five-quad scene mixes original sampled materials and RGB side walls under chromatic RGB illumination. |

The reference is the CPU Release/float build of [PBRT v4 at
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
Both renderers use 4,096 samples per pixel and 16 fixed replicates per scene,
with rypath seeds 0–15 and PBRT seeds 16–31. The comparison script changes the
authored seed for each replicate and uses PBRT's output override for linear
binary32 PFM. The rypath harness writes its production film projection in the
same format.

The comparison uses the full 8×8 frame for every scene and the four top-down
4×4 quadrants for both `multibounce.pbrt` and `rgb-multibounce.pbrt`. Each region's R, G, and B means must
agree within five combined standard errors plus
`2e-5 * max(1, abs(reference))`. See the [render profile](../../../docs/pbrt-rendering.md)
for the comparison command.
