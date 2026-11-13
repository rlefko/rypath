# PBRT spectral render fixture manifest

The thirteen rypath-authored entry points are licensed under GPL-3.0-only.
`cornell-box-adapted.pbrt` and `cornell-box-attributes.pbrt` are CC0 derivatives
of the [pinned Cornell Box fixture](../pbrt/README.md) by Benedikt Bitterli,
converted to PBRT v4 by Matt Pharr. The [upstream notice](../pbrt/LICENSE.txt)
applies to both derivatives.
All geometry and spectral values are inline. Each entry point expands to its
listed size with zero external dependencies (25,260 bytes total).

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
| `triangle-filter.pbrt` | 901 | `400cdd4baef8603ac2a848271040841f9d8f8a0a92451526713e383ce935d91d` | 2 meshes, 8 vertices, 4 emissive triangles |
| `normals-diffuse.pbrt` | 1,071 | `18575bec4cc1e7955ac10b3326d24080a724797d7989593c57fa414ebac279fa` | 2 meshes, 7 vertices, 3 triangles, 1 emissive |
| `normals-instances.pbrt` | 1,287 | `eb0cf9d98789e548bcf711e9a5145e46b66e45688b7be9c960a092338c8f1f89` | 2 meshes, 7 vertices, 3 triangles, 1 emissive, 2 instances of the receiver |
| `normals-emission.pbrt` | 1,114 | `223c9c317a36a915697509d83985e0524974fe7e6212be604dea338254063300` | 2 meshes, 8 vertices, 4 emissive triangles |
| `cornell-box-attributes.pbrt` | 7,542 | `3e0fd272dbd50a48adee7b3fa94b92b873a2f7b022e299d6420e88e924bd3d01` | 8 meshes, 72 vertices with normals and UVs, 36 triangles, 2 emissive |

Each scene uses a pinhole perspective camera, an 8×8 full-frame sRGB film
with the CIE 1931 sensor, ISO 100, the default shutter interval
[0, 1], `savefp16=false`, an independent sampler, and the path integrator.
`triangle-filter.pbrt` uses a triangle filter with radii (1.5, 0.75), and
`cornell-box-attributes.pbrt` preserves its authored triangle radii (1, 1).
The other thirteen use a box filter with radius 0.5 on each axis. Both Cornell
derivatives preserve the authored camera pose. The six background, diffuse,
and multibounce fixtures each use one uniform infinite light. Their RGB
variants exercise explicit RGB or omitted D65 emission.

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
| `triangle-filter.pbrt` | 53.1301023542° | 0 | Asymmetric RGB emitter edges against a uniform RGB environment exercise anisotropic edge smoothing, orientation, and normalization. |
| `normals-diffuse.pbrt` | 7.15266875° | 1 | Unequal-length vertex normals receive light across the geometric plane. |
| `normals-instances.pbrt` | 30° | 1 | An unscaled receiver shares its attributed mesh with a mirrored, scaled, and sheared receiver. |
| `normals-emission.pbrt` | 35° | 0 | Opposed normals and mirrored authored reversal orient two one-sided emitters toward the camera. |
| `cornell-box-attributes.pbrt` | 19.5° | 65 | Retains all authored normals and UVs, including the ceiling emitter's attributes. |

The native 1×1 coverage oracles separately verify samples beyond the nominal
film. This reference fixture does not independently detect clamping to the film
boundary because its outer emitter edges exceed the entire filter footprint.

Both Cornell derivatives add an attribution header, replace Sobol with
independent sampling (seed 0, 4,096 spp), change 1,024×1,024 to 8×8, set the CIE 1931
sensor and ISO 100 explicitly, and set `savefp16=false`. Geometry, winding, materials,
RGB emission, scoped state, camera transform, path depth of 65, and the authored
`cornell-box.png` filename carry over from the original.
`cornell-box-adapted.pbrt` replaces the radius-1 triangle filter with the
radius-0.5 box filter and removes eight authored normal arrays and eight UV
arrays. The removed normals agree with the geometric normal orientations and
differ slightly in direction. `cornell-box-attributes.pbrt` retains the
original bytes from `WorldBegin` onward, including all eight normal arrays and
all eight UV arrays. The reference script uses PBRT's `--outfile` and the rypath harness's positional output argument
to write PFM. The product CLI uses `--output` to select the PNG path.

The reference is the CPU Release/float build of [PBRT v4 at
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
Both renderers use 4,096 samples per pixel and 16 fixed replicates per scene,
with rypath seeds 0–15 and PBRT seeds 16–31. The comparison script changes the
authored seed for each replicate and uses PBRT's output override for linear
binary32 PFM. The rypath harness writes its production film projection in the
same format.

The comparison uses the full 8×8 frame for every scene and the four top-down
4×4 quadrants for both multibounce scenes, the three area scenes, the two
Cornell derivatives, the triangle filter scene, and the three owned normal
scenes. Each region's R, G, and B means must agree within five combined
standard errors plus
`2e-5 * max(1, abs(reference))`. See the [render profile](../../../docs/pbrt-rendering.md)
for the comparison command.
