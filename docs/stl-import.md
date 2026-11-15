# STL import, profile 1

The portable STL adapter checks triangle assets and converts them to a PBRT world
fragment with packaged PLY meshes. It supports ASCII and little-endian binary
STL, VisCAM/SolidView facet RGB, and Materialise Magics object/facet RGB and
object alpha. An STL asset does not specify a complete camera and lighting setup.

```sh
./build/debug/rypath --check-stl model.stl
./build/debug/rypath --check-stl colored.stl --stl-colors viscam
./build/debug/rypath --convert-stl colored.stl --output build/model --stl-colors magics
```

`--stl-colors` accepts `auto` (default), `viscam`, or `magics`, on either command.
Preflight creates no files and does not acquire a GPU. Both commands return 0
for success including warnings, 2 for unsupported behavior, and 1 for invalid
input, resource limits, or I/O errors. Errors take precedence.

## Triangle interpretation

`stl-triangle-v1` preserves authored coordinates, units, axes, winding and facets.
It does not weld vertices, infer units, close holes, orient connected components,
or repair topology. Open, disconnected and nonmanifold triangle soups are allowed.
Flat shading uses the geometric normal; PLY output omits redundant vertex normals
and does not invent UVs. Authored facet normals remain in provenance. Zero or
inconsistent normals produce aggregated warnings with counts and source locations.
Nonfinite normals or positions are errors.

An exact binary header/count/file-size match takes precedence over ASCII detection,
including binary headers beginning with `solid`. Binary fields are decoded
explicitly little endian: 80 header bytes, a 32-bit facet count, then 50 bytes per
facet. Truncated records, inconsistent counts and unexplained trailing data fail.
Otherwise the reader requires complete ASCII solid/facet/loop/vertex syntax.
Multiple solid blocks, including empty blocks, retain names and facet membership;
the complete asset must contain at least one facet. Source positions are checked
before and after binary32 narrowing; degeneracy, orientation loss, collapse,
nonzero underflow, and excessive scale-relative coordinate loss are errors.
The local coordinate-loss tolerance follows OBJ: `64 * FLT_EPSILON * diagonal`.
Triangles must also fit the existing native surface-normal and edge arithmetic
range at their authored scale. Extremely small or large facets that would
underflow or overflow binary32 normal evaluation fail preflight; coordinates are
not automatically rescaled. The enclosing scene's later transforms retain the
renderer’s existing numerical limitations.

## Vendor colors and transparency

The `stl-srgb-coverage-v1` conversion profile explicitly interprets vendor colors
as sRGB display values. Five-bit channels divide by 31, eight-bit channels by 255,
then the sRGB inverse transfer function produces linear RGB diffuse reflectance.
This is a declared model choice: STL does not describe a spectral material.
Uncolored facets use neutral linear RGB `.5`. Warnings and effective settings
are retained in `conversion.json`.

| Convention | Facet word and default behavior |
| --- | --- |
| VisCAM/SolidView | Bit 15 set selects facet RGB: red bits 10–14, green 5–9, blue 0–4. Bit 15 clear selects neutral diffuse; inactive low bits remain in provenance. |
| Materialise Magics | Bit 15 clear selects facet RGB: red bits 0–4, green 5–9, blue 10–14. Bit 15 set inherits the header `COLOR=RGBA` color. |

`auto` recognizes a complete `COLOR=` record in the 80-byte header as Materialise.
Without that header, all-zero words mean an uncolored asset; any nonzero word
requires an explicit convention. This avoids guessing between incompatible bit
meanings. Headerless all-black Materialise files therefore need `--stl-colors magics`.
Explicit Materialise without a header is supported only when every facet supplies
its own color. Its opacity defaults to one. Explicit VisCAM combined with a
Materialise header is an error, as are duplicate or truncated color records.
Binary vendor modes on ASCII input are unsupported.

Materialise header alpha is interpreted as opacity `A/255` and applies to all
facets, including facets with their own RGB. It maps to PBRT shape alpha and the
existing stochastic surface-coverage model, without premultiplying color.
Zero-alpha facets retain geometry and provenance. This models partial coverage;
it does not infer glass refraction or a participating medium. Richer `MATERIAL=`
records specifying diffuse/specular/ambient material properties are explicitly
unsupported, rather than silently reduced to color.

The primary implementation references are the pinned
[Assimp STL loader](https://github.com/assimp/assimp/blob/6bf4b3a4f0d1750c4a4cdc5a3dd0fef4ec0a666d/code/AssetLib/STL/STLLoader.cpp)
for binary framing and VisCAM channel layout, and
[Three.js r181](https://github.com/mrdoob/three.js/blob/r181/examples/jsm/loaders/STLLoader.js)
for Materialise validity/inheritance, object alpha and sRGB interpretation.
The rypath coverage mapping and strict ambiguity policy are our adaptations.
The implementations are references, not a claim of an authoritative unified
vendor standard or permission to copy code without its notices.

## Bundle and resource contract

Conversion requires a new directory whose parent exists. It writes a sibling
staging directory, validates the generated PBRT scene, and publishes by atomic
no-replacement rename. An existing file, directory or symlink remains unchanged;
failed conversion cleans up its staging directory. Atomic visibility does not
promise power-loss durability.

The bundle contains `model.pbrt`, `meshes/part-N.ply`, `conversion.json`, and
`README.txt`. A part contains one effective color/coverage combination. Parts use
first-use order, with original facet order within each part. Global grouping can
change emitted primitive order; explicit source-facet mappings retain provenance.
The manifest records source/output hashes, source normals and facet attributes,
solid membership, raw binary header bytes, requested/effective color conventions,
effective palette and coverage, locations, and diagnostics.

PBRT resolves dependencies relative to the entry scene directory. Place the
enclosing scene at the bundle root, include `model.pbrt`, and move the whole bundle
together. Use the [OBJ example wrapper](obj-import.md) with a camera appropriate
to the asset's authored scale; STL conversion does not guess a camera.

Input and generated files are bounded by the inherited 16 MiB per-file,
64 MiB aggregate input, two-million decoded-value and 100-diagnostic budgets.
The STL-owned working and output limits are 256 MiB each, including retained
source/provenance and serialization buffers. Generated PBRT validation retains
its separate scene budgets; this is not a process-memory guarantee.
Flat triangle PLY decoding charges 13 values per facet. Generated wrapper,
fragment, material and geometry values also consume their applicable budgets.
The default 256-file expansion budget leaves 254 PLY parts after the validation
wrapper and fragment. Richer palettes fail with a resource diagnostic; colors
are never quantized or merged to meet the limit. Limits are checked during
portable preparation as well as staged validation.

See [validation](stl-validation.md) for evidence on the tested revision and the
exact selected external asset coverage. Other mesh formats and broader STL
material extensions remain separately scoped work.
