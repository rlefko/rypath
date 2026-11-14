# PBRT spectral render fixture manifest

The rypath-authored entry points are licensed under GPL-3.0-only.
`cornell-box-adapted.pbrt`, `cornell-box-attributes.pbrt`, and the three
`cornell-box-sobol-*.pbrt` entry points are CC0 derivatives
of the [pinned Cornell Box fixture](../pbrt/README.md) by Benedikt Bitterli,
converted to PBRT v4 by Matt Pharr. The [upstream notice](../pbrt/LICENSE.txt)
applies to all five derivatives.
The 27 fixtures below use inline geometry and have no external dependencies
(57,608 bytes total). The [PLY asset manifest](ply-assets/README.md) documents
the standalone PLY wrappers and their inputs. [`dependencies.json`](dependencies.json)
pins asset hashes and sizes.

| Entry point | Bytes | SHA-256 | Geometry |
| --- | ---: | --- | --- |
| `background.pbrt` | 651 | `95612a748fa17e35844bd5ccd86c22d991281885fea2ab6a68675cc09583602b` | 1 mesh, 3 vertices, 1 triangle |
| `diffuse.pbrt` | 654 | `4a342f8759f744112076f72ef091cbbc90811040f0dd745c7f282ac129cceb71` | 1 mesh, 4 vertices, 2 triangles |
| `texture-diffuse.pbrt` | 777 | `f4d658dbece38feed2a9284fdf69e9861e7ba7b355f49d9452bd398be5045fbb` | Same geometry as `diffuse.pbrt` |
| `multibounce.pbrt` | 1,434 | `6a9df8ae504134824986df5bafe1e24d84939d0da4f17042f45a8b1495711cb3` | 5 meshes, 20 vertices, 10 triangles |
| `rgb-background.pbrt` | 657 | `0f550eebefcad23b0c3baa044739509a53772f9512604cd4fd527403d338d8d0` | 1 mesh, 3 vertices, 1 triangle |
| `rgb-diffuse.pbrt` | 614 | `ccce5d120a3f8625453873973cffb28ca7c158214259deb666f16b039a8c9f57` | 1 mesh, 4 vertices, 2 triangles |
| `rgb-multibounce.pbrt` | 1,380 | `afe3043c925b55a9bf37a0e637b743fcc861b7e52359f0448bfed584fd4e7754` | 5 meshes, 20 vertices, 10 triangles |
| `area-visible.pbrt` | 983 | `34d08504ae1eb4b19b960bfb19a5c2614effb0062c932e701211ff3ff50c4389` | 2 meshes, 8 vertices, 4 emissive triangles |
| `area-direct.pbrt` | 953 | `d9c22f5e038963e3491116b6685b6ed01c6126257cc517a277730ae4011ac94e` | 2 meshes, 7 vertices, 3 triangles, 1 emissive |
| `area-mixed.pbrt` | 1,726 | `a4cd3482f208a49e56e2672c238adedddd0c18523f12a0a54937b53a4820c176` | 6 meshes, 23 vertices, 11 triangles, 3 emissive |
| `blackbody-background.pbrt` | 657 | `d0b0e19230863d0068b1f0a3e7d58d2677e0532d7bfb941c566581e91fed3b0e` | Same geometry as `rgb-background.pbrt` |
| `blackbody-area.pbrt` | 956 | `18f3fe8a8ed4ab97d157c91cea13332a52fcc95e98c5a6d8cf210f056df5159e` | Same geometry as `area-visible.pbrt` |
| `blackbody-mixed.pbrt` | 2,022 | `30ae4424e942608342d96bd7629aff3ce60bbfdabc03b86133453c4e7af4083b` | 7 meshes, 25 vertices, 11 triangles, 3 emissive |
| `named-background.pbrt` | 666 | `a9b54b9303e1e0342b73e33b8129e7bab9fcf78b8179c89a168a05c21223a64e` | Same geometry as `blackbody-background.pbrt` |
| `named-area.pbrt` | 1,306 | `603e24000163099756a1aa50d13506ef558ca31f491c10aa5f46f528af5668c4` | 3 meshes, 11 vertices, 5 emissive triangles |
| `named-mixed.pbrt` | 2,042 | `1dde89fc9225f547781164e0bf287ac1e569175a662b2e413112e202f7aa0d4a` | Same geometry as `blackbody-mixed.pbrt` |
| `reflectance-named-diffuse.pbrt` | 603 | `a084cd147ed09469a561b7c0a2a105fbb326af31bbdb1db6b1cba1b67bd2e5e5` | Same geometry as `diffuse.pbrt` |
| `blackbody-reflectance-diffuse.pbrt` | 596 | `76a61392b9bb168fe3a891d2031569929ab660baa199191bbb060d98a4ee9dd2` | Same geometry as `diffuse.pbrt` |
| `cornell-box-adapted.pbrt` | 4,293 | `652e63dc4806c5d3a33e6560b654eaf775fa80ef9e1df8243ebe1a9494698aa6` | 8 meshes, 72 vertices, 36 triangles, 2 emissive |
| `triangle-filter.pbrt` | 901 | `400cdd4baef8603ac2a848271040841f9d8f8a0a92451526713e383ce935d91d` | 2 meshes, 8 vertices, 4 emissive triangles |
| `normals-diffuse.pbrt` | 1,071 | `18575bec4cc1e7955ac10b3326d24080a724797d7989593c57fa414ebac279fa` | 2 meshes, 7 vertices, 3 triangles, 1 emissive |
| `normals-instances.pbrt` | 1,287 | `eb0cf9d98789e548bcf711e9a5145e46b66e45688b7be9c960a092338c8f1f89` | 2 meshes, 7 vertices, 3 triangles, 1 emissive, 2 instances of the receiver |
| `normals-emission.pbrt` | 1,114 | `223c9c317a36a915697509d83985e0524974fe7e6212be604dea338254063300` | 2 meshes, 8 vertices, 4 emissive triangles |
| `cornell-box-attributes.pbrt` | 7,542 | `3e0fd272dbd50a48adee7b3fa94b92b873a2f7b022e299d6420e88e924bd3d01` | 8 meshes, 72 vertices with normals and UVs, 36 triangles, 2 emissive |
| `cornell-box-sobol-fastowen.pbrt` | 7,574 | `166c61e1136a87129ab2899545f5c128a6545ad912fd012c9dcc5c711dc53ada` | Same geometry as `cornell-box-attributes.pbrt` |
| `cornell-box-sobol-permutedigits.pbrt` | 7,579 | `dd20fb726bbe55a7f4145a9b6803fb33e7379334d01370ccfbf44bff17f53b53` | Same geometry as `cornell-box-attributes.pbrt` |
| `cornell-box-sobol-owen.pbrt` | 7,570 | `835cacf63c0eb73b3198d65d6d40f8194bdfdda32926332a64c8f2296fbc83e6` | Same geometry as `cornell-box-attributes.pbrt` |

The four file emission entry points use the original
[spectrum assets](spectrum-assets/README.md). The file reflectance entries
also use those assets. The external PLY wrappers load the unmodified
412-byte CC BY 2.0 quad by Yasutoshi Mori from the
[pinned Sportscar asset](ply-assets/README.md). Each wrapper uses the settings
and dependency paths listed below.

| Entry point | Bytes | SHA-256 | FOV | Path maxdepth | Inputs and coverage |
| --- | ---: | --- | ---: | ---: | --- |
| `file-background.pbrt` | 677 | `57040170c696fe3fe7eb5a7f758798987049528e9496e1ea914840a53b43c4ee` | 50° | 0 | `sky.spd` lights the unobstructed background at scale 1.7 and illuminance 7. |
| `file-area.pbrt` | 1,333 | `8e9961183e9f55a7bb98f5ab896cee7692aef4fe0242d86c58a711e5d2a98659` | 45° | 0 | `warm.spd` at scale 1.75 and two-sided `cool.spd` emit from opposite-facing triangle meshes. An off-camera triangle supplies the `endpoint.spd` payload for the native test. |
| `file-mixed.pbrt` | 2,126 | `06868f36e9a0625c011b719e4fd695c656728a737628fcbbb506cde2fbdd4c6f` | 58° | 5 | The `sky.spd` environment and three area emitters at scales 3, 1.5 and 0.6 illuminate an asymmetric room. Two emitters share `warm.spd`, and one uses `cool.spd`. |
| `file-ply-external.pbrt` | 816 | `aaee0bcf9d4b2713a1bb1af881c6f104f9a65ed24a0ac2cc80b496b09d8daa89` | 33.4° | 1 | The transformed quad in `ply-assets/sportscar-quad.ply` emits `cool.spd` at scale 1.25 under a `sky.spd` environment. |
| `reflectance-file-ply.pbrt` | 798 | `4b32ca7147ce6212a7b985224f806c69dc3d36ab6fc73965fb6703e9c88fce0b` | 33.4° | 1 | The transformed Sportscar quad uses the `paint.spd` constant texture under a flat sampled infinite light. |
| `reflectance-controls.pbrt` | 904 | `63a5b4322b752e3419990910444eefc59ea95d1956b6b64f687726dfb637ed9f` | 50° | 1 | Three placeholder triangles supply the `clamp.spd`, `narrow.spd`, and `wide.spd` materials for the native controls. |
| `blackbody-reflectance-ply.pbrt` | 834 | `b6e1657ac03f24645973d7f979183d58b1c1f54ea163a127a2471a100dae5148` | 33.4° | 1 | The transformed Sportscar quad uses an inherited 6500 K spectrum constant texture under a flat sampled infinite light. |
| `image-point.pbrt` | 1,009 | `9d2fea3e2d7f6059aec2202e0fbbb2492366c50762e7255b38a7a9cabdc8894d` | 53.1301023542° | 1 | A covering triangle pair under D65 illumination samples the pinned PNGSuite image at fractional LOD using point filtering and repeat wrap. |

Each scene uses a pinhole perspective camera, an 8×8 full-frame sRGB film
with the CIE 1931 sensor, ISO 100, the default shutter interval
[0, 1], `savefp16=false`, and the path integrator. The three Sobol entry points
use the randomization named in the filename. The remaining scenes use independent sampling.
`triangle-filter.pbrt` uses a triangle filter with radii (1.5, 0.75), and
`cornell-box-attributes.pbrt` and the three Sobol derivatives preserve the
authored triangle radii (1, 1). The remaining inline fixtures use a box filter with radius
0.5 on each axis. The Cornell derivatives preserve the authored camera pose.
The six background, diffuse,
and multibounce fixtures each use one uniform infinite light. Their RGB
variants exercise explicit RGB or omitted D65 emission.

| Entry point | FOV | Path maxdepth | Spectral and geometric coverage |
| --- | ---: | ---: | --- |
| `background.pbrt` | 50° | 0 | A triangle behind the camera leaves the primary rays unobstructed. The light spectrum peaks at 460 nm and uses scale 2 to exercise negative and above-one linear RGB. |
| `diffuse.pbrt` | 50° | 1 | An unobstructed quad covers the frame. The broad light spectrum illuminates a diffuse reflectance with a knot at 503.5 nm. |
| `texture-diffuse.pbrt` | 50° | 1 | The `diffuse.pbrt` reflectance comes from a spectrum constant texture declared after its named diffuse material. |
| `multibounce.pbrt` | 58° | 5 (default) | An asymmetric opening, five quads and four diffuse spectra exercise visibility and successive surface scattering. |
| `rgb-background.pbrt` | 50° | 0 | HDR RGB [4, 0.15, 1.2], scale 1.7 and illuminance 7 distinguish D65 normalization from tinted-spectrum normalization. |
| `rgb-diffuse.pbrt` | 50° | 1 | Chromatic RGB [0.08, 0.7, 0.31] on a covering quad under omitted-L D65 illumination. |
| `rgb-multibounce.pbrt` | 58° | 2 | The asymmetric five-quad scene mixes original sampled materials and RGB side walls under chromatic RGB illumination. |
| `area-visible.pbrt` | 45° | 0 | A one-sided RGB emitter faces the camera and a two-sided sampled-spectrum emitter faces away. |
| `area-direct.pbrt` | 7.15266875° | 1 | A finite off-axis triangle illuminates a diffuse receiver under a sampled spectrum. |
| `area-mixed.pbrt` | 58° | 5 | Three unequal-area emitting triangles use RGB or omitted D65 emission alongside an RGB environment in an asymmetric room. |
| `blackbody-background.pbrt` | 50° | 0 | A uniform 3000 K environment uses scale 1.7 and illuminance 7. |
| `blackbody-area.pbrt` | 45° | 0 | A one-sided 3000 K emitter with scale 1.75 faces the camera. A two-sided 6500 K emitter faces away. |
| `blackbody-mixed.pbrt` | 58° | 5 | A 5000 K environment and three unequal-area emitters at 2500 K, 6500 K, and 15000 K illuminate the asymmetric room. |
| `named-background.pbrt` | 50° | 0 | A uniform `stdillum-F11` environment uses scale 1.7 and illuminance 7. |
| `named-area.pbrt` | 45° | 0 | A one-sided `glass-F10` emitter with scale 1.75 faces the camera. A two-sided `canon_eos_5d_b` emitter faces away. An off-camera `metal-MgO-k` emitter provides a narrow-band payload for the native test. |
| `named-mixed.pbrt` | 58° | 5 | An `illum-acesD60` environment and `stdillum-A`, `stdillum-F8`, and `metal-Au-eta` emitters illuminate an asymmetric room. |
| `reflectance-named-diffuse.pbrt` | 50° | 1 | Direct `metal-Au-eta` diffuse reflectance on a covering plane under a flat sampled infinite light. |
| `blackbody-reflectance-diffuse.pbrt` | 50° | 1 | Direct 3000 K blackbody diffuse reflectance on a covering plane under a flat sampled infinite light. |
| `cornell-box-adapted.pbrt` | 19.5° | 65 | Two downward-facing triangles emit from the ceiling fixture above two boxes and colored walls. |
| `triangle-filter.pbrt` | 53.1301023542° | 0 | Asymmetric RGB emitter edges against a uniform RGB environment exercise anisotropic edge smoothing, orientation, and normalization. |
| `normals-diffuse.pbrt` | 7.15266875° | 1 | Unequal-length vertex normals receive light across the geometric plane. |
| `normals-instances.pbrt` | 30° | 1 | An unscaled receiver shares its attributed mesh with a mirrored, scaled, and sheared receiver. |
| `normals-emission.pbrt` | 35° | 0 | Opposed normals and mirrored authored reversal orient two one-sided emitters toward the camera. |
| `cornell-box-attributes.pbrt` | 19.5° | 65 | Retains all authored normals and UVs, including the ceiling emitter's attributes. |
| `cornell-box-sobol-*.pbrt` | 19.5° | 65 | Same spectral and geometric coverage as `cornell-box-attributes.pbrt`, with three Sobol randomizations. |

The native 1×1 coverage oracles separately verify samples beyond the nominal
film. This reference fixture does not independently detect clamping to the film
boundary because its outer emitter edges exceed the entire filter footprint.

All Cornell derivatives add an attribution header, author sampler-local seed 0,
change 64 to 4,096 samples per pixel and 1,024×1,024 to 8×8, set the CIE 1931
sensor and ISO 100 explicitly, and set `savefp16=false`. Geometry, winding, materials,
RGB emission, scoped state, camera transform, path depth of 65, and the authored
`cornell-box.png` filename carry over from the original.
`cornell-box-adapted.pbrt` and `cornell-box-attributes.pbrt` replace Sobol with
independent sampling. The three Sobol derivatives keep the sampler and
explicitly set `randomization` to `fastowen`, `permutedigits`, or `owen`.
`cornell-box-adapted.pbrt` replaces the radius-1 triangle filter with the
radius-0.5 box filter and removes eight authored normal arrays and eight UV
arrays. The removed normals agree with the geometric normal orientations and
differ slightly in direction. `cornell-box-attributes.pbrt` and the three Sobol
derivatives retain the original bytes from `WorldBegin` onward, including all
eight normal arrays and all eight UV arrays. The reference script uses PBRT's
`--outfile` and the rypath harness's positional output argument
to write PFM. The product CLI uses `--output` to select the PNG path.

The reference is the CPU Release/float build of [PBRT v4 at
`b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
Both renderers use 4,096 samples per pixel and 16 fixed replicates per scene,
with rypath seeds 0–15 and PBRT seeds 16–31. The comparison script changes the
authored seed for each replicate and uses PBRT's output override for linear
binary32 PFM. The rypath harness writes its production film projection in the
same format. Sobol randomization changes the transport variates, and the
unscrambled camera sequence stays fixed across all replicates. The Sobol
standard errors describe the measured variation conditional on those camera
samples.

The native Metal test also compares `texture-diffuse.pbrt` with `diffuse.pbrt`
and `texture-ply-external.pbrt` with `ply-external.pbrt`. Each pair uses the same
reflectance, geometry and illumination. The test checks the translated
reflectance payloads and requires exact equality of the raw compensated films
and sample counts at the same seed, using 8×8 pixels, 64 samples per pixel and
32-sample batches. Zeroing the direct material's reflectance must change the
film, so the equality check exercises surface reflection.

The native Metal reflectance controls compare each path to its paired path
on a white diffuse plane. Independent evaluations use the original fractional
gold eta knots and analytic curves for the three file materials. The tests
check exact zero and white radiance where expected, along with raw film and
sample count equality across batch sizes 65, 7, and 1.

The comparison uses the full 8×8 frame for every scene. The
[`REGIONS` table](../../../scripts/compare_pbrt.py) defines additional regions,
including the four top-down 4×4 quadrants for the blackbody, named-spectrum
and file-spectrum area, mixed, and PLY scenes, plus the signed file and blackbody
reflectance PLY wrappers. Each region's R, G, and B means
must agree within five combined
standard errors plus
`2e-5 * max(1, abs(reference))`. See the [render profile](../../../docs/pbrt-rendering.md)
for the comparison command.

The point fixture uses a triangle filter with radii (1/256, 1/256). It also
compares the two pixels in column 4, rows 3–4, using zero-based top-down
coordinates. A same-seed bilinear control must reduce their red mean by more
than 0.02 on each backend. The [image asset manifest](image-assets/README.md)
records provenance and the mip footprint.
