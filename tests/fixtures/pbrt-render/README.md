# PBRT spectral render fixture manifest

These three entry points were authored for rypath and are licensed under
GPL-3.0-only. All geometry and spectral values are inline. The complete input
dependency closure is the three scene files below (2,739 bytes total, zero
external dependencies). Each entry point expands to its listed size.

| Entry point | Bytes | SHA-256 | Geometry |
| --- | ---: | --- | --- |
| `background.pbrt` | 651 | `95612a748fa17e35844bd5ccd86c22d991281885fea2ab6a68675cc09583602b` | 1 mesh, 3 vertices, 1 triangle |
| `diffuse.pbrt` | 654 | `4a342f8759f744112076f72ef091cbbc90811040f0dd745c7f282ac129cceb71` | 1 mesh, 4 vertices, 2 triangles |
| `multibounce.pbrt` | 1,434 | `6a9df8ae504134824986df5bafe1e24d84939d0da4f17042f45a8b1495711cb3` | 5 meshes, 20 vertices, 10 triangles |

Each scene uses an identity pinhole perspective camera, an 8×8 full-frame
sRGB film with the CIE 1931 sensor, ISO 100, the default shutter interval
[0, 1], `savefp16=false`, a box filter of radius 0.5 on both axes, an
independent sampler, and the path integrator. All three use one spatially
uniform infinite light with an inline numeric spectrum.

| Entry point | FOV | Path maxdepth | Spectral and geometric coverage |
| --- | ---: | ---: | --- |
| `background.pbrt` | 50° | 0 | A triangle behind the camera leaves the primary rays unobstructed. The light spectrum peaks at 460 nm and uses scale 2 to exercise negative and above-one linear RGB. |
| `diffuse.pbrt` | 50° | 1 | An unobstructed quad covers the frame. The broad light spectrum illuminates a diffuse reflectance with a knot at 503.5 nm. |
| `multibounce.pbrt` | 58° | 5 (default) | An asymmetric opening, five quads and four diffuse spectra exercise visibility and successive surface scattering. |

The reference is the CPU Release/float build of [PBRT v4 at
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
Both renderers use 4,096 samples per pixel and 16 fixed replicates per scene,
with rypath seeds 0–15 and PBRT seeds 16–31. The comparison script changes the
authored seed for each replicate and uses PBRT's output override for linear
binary32 PFM. The rypath harness writes its production film projection in the
same format.

The comparison uses the full 8×8 frame for every scene and the four top-down
4×4 quadrants for `multibounce.pbrt`. Each region's R, G, and B means must
agree within five combined standard errors plus
`2e-5 * max(1, abs(reference))`. See the [render profile](../../../docs/pbrt-rendering.md)
for the comparison command.
