# Polygonal OBJ/MTL import, profile 1

OBJ describes mesh assets, not a complete camera and lighting setup. The portable
importer checks an asset and converts it to a PBRT world fragment with packaged
meshes and textures. Rendering still uses the existing PBRT and Metal pipeline.

```sh
./build/debug/rypath --check-obj path/to/model.obj
./build/debug/rypath --convert-obj path/to/model.obj --output build/model
```

Conversion requires a **new directory** whose parent exists. It writes and checks
a sibling staging directory, then publishes with an atomic no-replacement rename.
An existing file, directory, or symlink is never replaced. Failure cleans up the
staging directory. This guarantees atomic visibility, not power-loss durability.
Preflight does not publish files or acquire a GPU. Both commands return 0 on
success, including approximation warnings; 2 for unsupported behavior; and 1 for
invalid input, resource limits, or I/O failure. Errors take precedence.

The bundle contains `model.pbrt`, triangle PLYs in `meshes/`, content-addressed
original/derived images in `assets/`, and `conversion.json`. The manifest records
source and output hashes, authored material properties, polygon object/group and
smoothing metadata, triangle-to-polygon associations, and conversion diagnostics.
Source assets retain their own terms; conversion does not assign a new license.

PBRT resolves assets relative to the **entry scene directory**, not the included
fragment. Put the enclosing scene at the bundle root and move the entire bundle
together. For a centered asset of roughly unit size, create `build/model/scene.pbrt`:

```pbrt
LookAt 0 0 -4  0 0 0  0 1 0
Camera "perspective" "float fov" [45]
Film "rgb" "integer xresolution" [256] "integer yresolution" [256]
    "string sensor" "cie1931" "bool savefp16" [false]
PixelFilter "box"
Sampler "independent" "integer pixelsamples" [64]
Integrator "path" "integer maxdepth" [8]
WorldBegin
LightSource "infinite"
Include "model.pbrt"
```

```sh
./build/debug/rypath --render-pbrt build/model/scene.pbrt --output build/model.exr
```

Adjust the camera for the asset's authored scale. Conversion preserves coordinate
axes, units, and winding; it does not guess a camera or change handedness.

## Geometry and source interpretation

`obj-polygon-v1` retains independent position/UV/normal indices, positive and
record-relative negative indices, polygon boundaries, material assignments,
objects, multiple group memberships, and smoothing IDs. Material libraries are
resolved relative to the OBJ; images are resolved relative to their declaring
MTL. No working-directory fallback or filename guessing is applied. Missing
libraries/images and undefined or duplicate materials are errors.

Simple planar convex and concave polygons are triangulated with preserved
boundary corners and attributes. Collinear boundary corners are retained through
edge splitting. Repeated edges, self-intersection, zero area, nonplanarity outside
the declared tolerance, and invalid geometry after binary32 narrowing are errors. Relative coordinate loss
and narrowed 3D planarity are checked before triangles are accepted.
The planarity tolerance is `64 * FLT_EPSILON * bounding_box_diagonal`. Triangulation
is a geometric conversion, not PBRT's bilinear-quad interpretation.

Authored normals are normalized and take precedence. Missing normals use the
original polygon normal for `s off`, or area-weighted incident polygon normals
keyed by source position and smoothing ID. Group, object, material and UV seams
do not independently break smoothing. Mixed authored/missing normals are an
explicit extension. Cancelling or zero generated normals are rejected. Mapped
materials require UVs at every corner; missing coordinates are never replaced
with `(0,0)`.

The adapter uses a pinned TinyObjLoader C11 callback decoder with strict project
record validation, owned provenance and bounded allocation. Earcut receives only
validated planar rings; output is independently checked before publication.
[Dependency records](dependencies.md) identify source pins and adaptations.

The inherited default limits are 16 MiB per source/mesh file, 64 MiB aggregate
input bytes, 256 input files, two million values, and 100 diagnostics. The OBJ
profile additionally caps faces at 4,096 corners, the sum of squared face arities at
2^26, and OBJ-owned working/output storage at 256 MiB each. Working accounting
includes retained inputs, metadata and publication buffers. The inherited PBRT
preflight/translation used to validate a staged bundle retains its separate
scene/image budgets; this is not a 256 MiB process-memory guarantee. Image
processing also follows the existing 4,096-pixel dimension and bounded codec
budgets. Limit failures are explicit; they do not drop faces or textures.

## Material conversion

`mtl-pbr-approx-v1` is a documented approximation policy. Warnings are emitted by
default and retained in `conversion.json`; no approval flag is required. It does
not claim that legacy Phong shading and a physical BSDF are equivalent. Conversion
uses the existing [PBRT rendering profile](pbrt-rendering.md), including its finite
coating and RGB-to-spectral model limitations.

| Input | Effective behavior |
| --- | --- |
| `Kd`, `map_Kd` | Base/diffuse color; multiply authored RGB into decoded color texels before spectral reconstruction |
| `illum 0/1` | Diffuse; mode 0 loses its original unlit behavior, with a warning |
| `illum 2` | Diffuse/coated-diffuse mixture using luminance of `Ks`/`map_Ks`; colored highlight loss is reported |
| `illum 3/5/8` | Conductor using the documented reflectance-to-optical-constants approximation |
| `illum 4/9` | Thin dielectric without absorption |
| `illum 6/7` | Dielectric using `Ni`, without inferred volume absorption |
| `Ns`, `map_Ns` | Blender-style heuristic `r = 1 - sqrt(clamp(Ns,0,1000)/1000)`; GGX alpha is `r*r` with `remaproughness=false` |
| `Pr`, `map_Pr` | Perceptual roughness overrides legacy `Ns`; alpha is `Pr*Pr` |
| `Pm`, `map_Pm` | Mix dielectric and conductor branches using base color |
| `Pc`, `Pcr` | One effective finite coating; overrides legacy coating controls and records the loss of separate highlights |
| `d`, `Tr`, `map_d`, `map_Tr` | Explicit stochastic surface coverage; `Tr` is inverted after its map multiplier; contradictory dissolve/transparency inputs fail |
| `Ke`, `map_Ke` | Diffuse area emission; only fully opaque emitting surfaces are supported |
| `Ka`, `map_Ka` | Retained legacy ambient information with a warning; never converted to emission |
| `bump`, `map_bump`, `norm` | Scalar bump or tangent-space +Y normal map; normal maps take precedence with a warning |

Numeric RGB is linear sRGB. Byte color images use sRGB decoding; floating color
images and scalar/normal maps are linear. A map without an authored multiplier
uses one. Unmapped defaults are diffuse .5, perceptual roughness .5, IOR 1.5,
coverage 1, and zero specular/metallic/coating/emission. Base-color image alpha
never implicitly changes coverage. Normal JPEGs are prepared as linear EXRs to
avoid the PBRT JPEG normal-map transfer convention.

Color and scalar maps support `-o`, `-s`, `-clamp`, and `-mm`. Scalar maps support
`-imfchan r/g/b/m/l` (matte requires retained alpha; default is linear luminance).
Bump maps support `-bm`. Derived scalar images have equal RGB channels so their
meaning does not depend on PBRT's filter-specific scalar-channel selection.
Preparation orders decoding, channel selection, base/gain, authored multiplier,
nonlinear roughness conversion, and then filtering. Normal and emission image
consumers require identity repeat UV mapping; unsupported transforms fail rather
than baking a different periodic function.

## Explicit exclusions

This is a polygonal profile, not full-spec OBJ. Points, lines, free-form curves
and surfaces are recognized as unsupported. Historical `call` expansion is
unsupported and `csh` execution is disabled. Unknown meaningful records/options
are diagnosed rather than silently discarded.

Shadow-matte illumination, nonwhite transmission filters, sheen, anisotropy
extensions, true displacement, reflection/environment maps, spectral/XYZ MTL
colors, and incompatible texture options remain unsupported. Nonopaque emission
includes **zero** opacity: accepting that combination would trigger PBRT's
invisible-emitter behavior rather than an ordinary fully dissolved asset.
Transmission with unsupported metallic/coating combinations is diagnosed.

The heuristic follows the pinned [Blender importer](https://github.com/blender/blender/blob/4223993fe4f6b45cd305ac1f15c5fb3fa5383eb8/source/blender/io/wavefront_obj/importer/obj_import_mtl.cc).
Selected extensions follow the [PBR MTL proposal](https://ben3d.ca/blog/extended-wavefront-obj-mtl-for-pbr).
These sources define inputs and prior conversion practice; the particular rypath
material graph is our declared adaptation. See [validation](obj-validation.md)
for tests actually run and selected external-asset coverage.
