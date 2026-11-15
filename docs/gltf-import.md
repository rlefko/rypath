# Static glTF/GLB import, profile 1

`gltf-static-v1` converts a selected static glTF 2.0 scene into a relocatable
PBRT world fragment. Its `rypath-gltf-v1` material and image-sampler extensions
preserve core metallic-roughness inputs for rypath's spectral adaptation.
Generated scenes require rypath; stock PBRT compatibility is not claimed.

```sh
./build/debug/rypath --check-gltf model.gltf
./build/debug/rypath --check-gltf model.glb --gltf-scene 1
./build/debug/rypath --convert-gltf model.glb --output build/model
```

Both commands are portable and require no GPU. Exit 0 means success including
warnings, 2 means unsupported behavior, and 1 means malformed input, missing
required data, a resource limit or an I/O failure. Preflight writes no files.
Diagnostics retain source paths, JSON locations and semantic context.

## Selection and geometry

An explicit `--gltf-scene INDEX` selects a zero-based scene. Otherwise the asset's
default scene is used; a single scene needs no default. Multiple scenes without a
default require explicit selection. The selected scene must place supported
nonempty triangle geometry.

| Input | Profile |
| --- | --- |
| JSON `.gltf` and version-2 `.glb` | Checked framing, strict finite JSON, Unicode decoding, duplicate-key rejection, bounded nesting and tokens |
| Buffers | Relative local files, supported base64 data URIs, or GLB BIN; checked immutable byte snapshots |
| Accessors | Checked offsets, alignment, strides and sparse overrides, including tightly packed sparse values over interleaved base data |
| Geometry | Indexed and nonindexed `TRIANGLES`; unsigned 8/16/32-bit indices; FLOAT positions with enclosing min/max |
| Attributes | FLOAT normals and signed FLOAT tangents; one active UV set shared by the material's rendered textures, FLOAT or normalized unsigned 8/16-bit UVs |
| Missing normals/tangents | Flat per-face normals; MikkTSpace tangent generation for normal-mapped geometry when needed, with per-corner frame splits |
| Scene placement | Node hierarchy, affine matrix or TRS, mesh reuse, nonuniform and reflected placements |
| Images | Selected PNG/JPEG sources from relative files, supported data URIs or image bufferViews |

Coordinates, axes and winding are preserved. There is no automatic unit change,
vertex welding, topology repair or degenerate-face removal. Source triangles,
binary32 mesh storage and placed triangles are checked for invalid numeric ranges
and collapse. Normals must be unit length within the declared numeric tolerance;
active tangents must have unit XYZ and a sign of exactly +1 or -1. Invalid or
parallel normal/tangent frames fail instead of receiving an invented frame.

Canonical UVs and serialized PLY UVs remain native glTF `(u,v)`, with a top-left
texture origin. glTF lookups bypass PBRT's V flip. MikkTSpace receives those same
coordinates. Normal maps receive no automatic green-channel inversion.
The bitangent uses the signed tangent frame; reflected transforms account for
handedness once. Camera and shadow sidedness use geometric winding independently
of authored or mapped shading normals.

## Material and texture fidelity

The model target is the core dielectric/metal branch mixture in
[Khronos glTF-Sample-Renderer at `623715005b00f5dba155e7448706bf87202e553a`](https://github.com/KhronosGroup/glTF-Sample-Renderer/tree/623715005b00f5dba155e7448706bf87202e553a),
with the [glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)
as the format reference. This is a declared RGB-to-spectral adaptation, rather
than measured spectral material recovery or pixel equivalence with that renderer.

Color textures decode sRGB before filtering and multiplication by linear RGB
factors. The resulting RGB is reconstructed spectrally after multiplication.
Packed metallic/roughness data uses B/G respectively, multiplied by its factors;
normal and alpha data remain linear. Emission uses its RGB factor and optional
sRGB texture, using the same D65 photometric normalization as PBRT RGB image
lights in this renderer. Ambient-occlusion images are checked and retained in provenance,
but ignored by path transport with a warning.

The dedicated BSDF mixes dielectric Lambert reflection and isotropic GGX/Smith
reflection with Schlick Fresnel, dielectric F0 .04 and metallic base color.
Perceptual roughness squares to GGX alpha. Exactly zero roughness uses a delta
specular lobe; positive roughness is not silently remapped to a smooth material.
Finite arithmetic failures remain errors. The literal reference branch mixture
can exceed unit directional reflectance: the independent grazing, zero-roughness
white dielectric probe integrates to about 1.719. Pure rough metals also lose
energy through omitted multiple scattering. We preserve and disclose these
reference-model limitations; this is not a physically energy-conserving material
model in every configuration. This single-scattering model has no
multiple-scattering compensation, transmission, clearcoat or extension lobes.
Visible-normal sampling and the diffuse/specular selection PDF belong to this
model; ordinary PBRT material interpretation remains separate.

All core sampler modes are supported: independent S/T repeat, mirrored-repeat or
clamp-to-edge; nearest or linear magnification; nearest/linear base filtering and
all four nearest/linear mip combinations for minification. Defaults are repeat,
linear magnification and linear-mipmap-linear minification. Native NPOT dimensions
and linear floating-point RGBA mip chains are retained. Alpha remains
unassociated; RGB is not premultiplied by coverage.

`OPAQUE` ignores alpha. `MASK` accepts alpha greater than or equal to its cutoff,
after image sampling and factor multiplication; cutoff zero and values above one
retain their literal meaning. `BLEND` uses stochastic surface coverage without
an extra throughput factor, approximating compositing through path visibility.
It does not infer refractive glass or volume absorption. Visibility, including
shadow rays and emitter coverage, uses LOD zero to keep geometric visibility
independent of the shading footprint. Color, packed data and normal shading use
the texture-footprint sampler policy.

Single-sided geometry culls back faces; double-sided shading reverses the
backface frame. OPAQUE and MASK materials may emit, including textured emission
and masked emitters. A BLEND emitter is accepted only when its factor and every
base-image alpha sample prove it opaque. Fractional BLEND emission is unsupported.
Emitters are emitted as direct placed geometry rather than object-definition
members. Emission sidedness follows geometry, independently of smooth normals.

## Explicit PBRT extensions

`Material "rypath-gltf-v1"` and named materials with
`"string type" "rypath-gltf-v1"` accept these fields:

| Typed field | Default / meaning |
| --- | --- |
| `rgb basecolor` | `[1 1 1]`, linear RGB factor in [0,1] |
| `float opacity` | `1`, base-color alpha factor in [0,1] |
| `float metallic` | `1`, factor in [0,1] |
| `float roughness` | `1`, perceptual factor in [0,1] |
| `float normalscale` | `1`, finite signed scale of tangent-space X/Y |
| `rgb emissive` | `[0 0 0]`, linear RGB factor in [0,1] |
| `string alphamode` | `"OPAQUE"`, `"MASK"` or `"BLEND"` |
| `float alphacutoff` | `.5`, finite nonnegative cutoff |
| `bool doublesided` | `false` |
| `texture basecolortexture` | Optional named glTF spectrum imagemap, sRGB |
| `texture metallicroughnesstexture` | Optional named glTF spectrum imagemap, linear |
| `texture normaltexture` | Optional named glTF spectrum imagemap, linear; requires signed tangent frames |
| `texture emissivetexture` | Optional named glTF spectrum imagemap, sRGB |

The material supports triangle geometry only; bilinear patches are rejected.
Image slots bind RGB images directly, rather than evaluating a spectral texture
graph before factor multiplication. Standard textures cannot substitute for
these slots; glTF imagemaps cannot be used in ordinary material graphs. These
materials cannot be mix children, use independent shape alpha, or combine with
`AreaLightSource`. Nonopaque coverage cannot cross a medium interface.

```pbrt
Texture "color" "spectrum" "imagemap" "string filename" "images/color.png"
    "string encoding" "sRGB" "bool rypath.gltf" [true]
    "integer rypath.wraps" [10497] "integer rypath.wrapt" [10497]
    "integer rypath.minfilter" [9987] "integer rypath.magfilter" [9729]
MakeNamedMaterial "surface" "string type" "rypath-gltf-v1"
    "texture basecolortexture" "color" "float metallic" [.5]
```

Sampler integers retain the glTF/OpenGL enums: repeat 10497, mirrored-repeat
33648, clamp-to-edge 33071; nearest 9728, linear 9729; minification also accepts
9984–9987. The `rypath.gltf` marker is required. Standard imagemap mapping, filter,
wrap, scale and inversion controls cannot be mixed with this policy. Color/data
encoding must match the material slot.

PLY files with signed frames declare `comment rypath_gltf_tangent_frame_v1` and
scalar `tangent_x`, `tangent_y`, `tangent_z`, `tangent_sign` properties. Complete
normals/tangents/signs are required, with one sign per vertex and consistent signs
within each triangle. Without the marker these names remain ordinary ignored PLY
properties, preserving existing PLY interpretation.

## Conservative input boundaries

Structural checks cover the entire asset's declared JSON structure, reference
ranges, node hierarchy and buffer/accessor storage, including unused scenes.
All declared buffers are resolved and range-checked. Selected geometry, active
texture accessors and selected images then receive semantic decoding and numeric
validation. A structurally valid unused animation or unsupported topology does
not by itself make the selected static scene animated or renderable. This is
neither a full glTF validator nor a claim that unused payloads were rendered.

Required extensions are rejected. Extensions on selected rendering objects are
rejected instead of silently discarded, including compression, material/texture
extensions and texture transforms. Selected animation targets, skins, morphs,
vertex colors, skeletal/custom attributes and nontriangle modes are unsupported.
Rendered texture slots must share one UV set; arbitrary multiple UV consumers are
not implemented. Selected cameras are retained in source provenance with a
warning: the bundle deliberately leaves camera and lighting to its caller.

Local resource URIs are percent-decoded UTF-8 relative paths. Absolute paths,
network schemes, queries, fragments, backslashes and control characters are
rejected. A decoded `..` path component is deliberately unsupported, even when
it would resolve to an existing local file. This is an importer restriction,
not a claim that such glTF URIs are invalid. Unknown GLB chunks are likewise
conservatively unsupported rather than skipped. GLB JSON/BIN order, uniqueness,
lengths and permitted padding sizes are checked. No dependency is downloaded or executed.

Limits bound input/expanded bytes, files, JSON structure, decoded values, image
storage, retained working memory, output size and algorithmic work. Defaults
include 16 MiB per input file, 64 MiB expanded input, 256 MiB working and output
budgets, and 2^26 work items. MikkTSpace conservatively reserves quadratic face
work, so missing authored tangents can exhaust the default work budget before
8,192 normal-mapped triangles. Resource exhaustion fails visibly without partial
geometry. See the [dependency records](dependencies.md) for adaptation boundaries.

## Bundles, examples and evidence

Conversion requires a new destination directory with an existing parent. It
prepares and validates generated files before publishing through a sibling
staging directory and atomic no-replacement rename. Existing destinations remain
unchanged. The bundle contains `model.pbrt`, checked PLY meshes, copied image
snapshots, instructions and `conversion.json` with source/resource/output hashes,
selection, names, primitive/placement mappings, effective factors/samplers and
warnings. Original assets retain their licenses. No source reread is needed for
publication. Keep an enclosing PBRT scene at the bundle root, then
`Include "model.pbrt"` after `WorldBegin`; move the whole directory together.

The owned [showcase generator](../scripts/create_gltf_showcase.py) creates a
metallic/roughness sphere study and a textured GLB study with native UVs, normal
detail, mirrored placement, MASK perforations and BLEND coverage. It invokes the
actual CLI converter and writes hashes/settings outside Git:

```sh
python3 scripts/create_gltf_showcase.py --rypath build/debug/rypath \
    --output-dir .cache/showcase/gltf-run --samples 256 --size 960 --render
```

Current focused M4 Max evidence includes 4,528 GPU BSDF
comparisons against independent double equations, 2,097,152 Monte Carlo samples,
and 36,311 GPU texture cases. Debug, Release and ASan/UBSan each pass 163 tests;
see the [validation record](gltf-validation.md) for full-render comparisons and
acceptance thresholds. The two small pinned
[Khronos assets](../tests/fixtures/gltf/external/README.md) establish selected
parser/bundle coverage, not full-corpus or external-renderer image equivalence.
Earlier renderer/import validation records describe their own historical revisions.
