# PBRT v4 structural preflight, profile 1

`rypath --check-pbrt SCENE` analyzes a plain-text PBRT v4 entry point and prints
source-located diagnostics plus separate syntax, checked-structure, dependency,
and unsupported-feature summaries. Rendering is never attempted and no images
or scene outputs are created. This command works without Metal.

| Exit | Meaning |
| --- | --- |
| 0 | Analysis completed within this structural profile |
| 2 | Recognized features are outside this profile |
| 1 | Malformed input, missing required files, unresolved references, invalid checked geometry, or a resource limit |

Errors take precedence over unsupported features. A complete checked scene is
withheld when analysis is incomplete. The report may still show a partial
inventory; its counts are not evidence that unchecked content was processed.

## Compatibility matrix

The reference is [PBRT v4 at b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f](https://github.com/mmp/pbrt-v4/tree/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f).
“Checked” below means structural interpretation. The Rendering column describes
`--check-pbrt`, which never renders. The separate [spectral rendering
profile](pbrt-rendering.md) defines the narrower `--render-pbrt` contract.

| Feature | Syntax / authored data | Structural interpretation | Rendering |
| --- | --- | --- | --- |
| Current v4 directive forms and typed scalar/array parameters | Checked, with owned locations and numeric lexemes | Type/cardinality and supported descriptor schemas checked | Not attempted |
| Static affine transforms, named coordinates, scopes, orientation | Retained | Column-major input, postmultiplication, world reset, scoped state | Not attempted |
| Perspective camera, RGB film, box/triangle filter, independent or Sobol sampler, path/volpath integrator, BVH | Retained | Declared parameter schemas checked | See the separate [render profile](pbrt-rendering.md) |
| Diffuse, conductor, dielectric and extended surface materials and named bindings | Retained | Last matching typed parameter wins; dielectric float eta precedes spectral eta; explicit values override inherited defaults | Not translated |
| Named float and spectrum constant textures | Retained with authored and inherited values and color-space provenance | Separate global namespaces; typed diffuse reflectance references resolve after parsing | See [constant textures](pbrt-rendering.md#constant-textures) |
| Typed texture graphs and PNG/JPEG/EXR/HDR imagemaps | Retained with effective parameters and declaration transforms | Typed forward references, cycle/work limits, entry-relative decode and mip validation, including unused definitions | See [core texturing](pbrt-rendering.md#core-texture-graphs-and-consumers) |
| Constant and image infinite-light descriptors | Retained | Descriptor and reference checks | No normalization or light sampling |
| Point/spot intensity, power, position, cone and declaration transform | Retained, including inherited parameters and endpoint medium | Checked spectral dependencies, finite parameters, spot angle/endpoint domains and a combined 64 positional-light limit with goniometric and projection sources | See the [point](pbrt-rendering.md#point-lights) and [spot](pbrt-rendering.md#spot-lights) profiles |
| Goniometric intensity, power, scalar image and declaration transform | Retained with inherited parameters and endpoint medium | Required nonempty square PNG/JPEG/EXR/HDR image, prepared gray or RGB/RGBA scalar data, checked spectral dependencies and the combined 64 positional-light limit | See the [goniometric profile](pbrt-rendering.md#goniometric-lights); translation additionally restricts transforms and numerical range |
| Projection image, FOV, power and declaration transform | Retained with inherited parameters and endpoint medium | Required nonempty RGB/RGBA PNG/JPEG/EXR/HDR image, finite scale/power, valid FOV and the combined 64 positional-light limit | See the [projection profile](pbrt-rendering.md#projection-lights) for affine and numerical limits |
| Distant radiance, illuminance, direction and declaration transform | Retained with inherited parameters | Checked spectral dependencies, finite distinct endpoints and a separate 64-light limit | See the [distant profile](pbrt-rendering.md#distant-lights); translation additionally requires valid scene bounds |
| Diffuse constant and image area-light declarations | Retained | Scoped shape binding, defaults, reset, and descriptor checks | No normalization or light sampling |
| Color spaces, RGB, blackbody, sampled spectra, built-in spectrum names | Retained | Types and references checked | No resampling, clamping, or reconstruction |
| File spectra on diffuse/conductor/dielectric materials, homogeneous media, spectrum constants, and supported lights | Retained with parameter provenance | Entry-relative bounded decode into shared binary32 knots | See the separate [render profile](pbrt-rendering.md) |
| Inline trianglemesh, external triangular/quad PLY and single-level object instances | Retained | Converted to checked float/index triangle/bilinear geometry | Not submitted to Metal |
| Plain-text Include | Retained and expanded | Shared scene state, entry-directory resolution | Not applicable |
| Import and gzip scenes | Recognized | Unvisited; dependency inventory incomplete | Unsupported |
| Other external meshes, image formats and textures outside the core profile, other file-spectrum consumers | Recognized; discoverable paths reported | Content unchecked; missing inputs are errors | Unsupported |
| Homogeneous media and medium interfaces | Retained with named definitions, inherited parameters and declaration transforms | Scoped shape bindings and declaration-time camera outside medium; references checked | See [homogeneous media](pbrt-rendering.md#homogeneous-media) |
| Emitting object members, other shapes/materials, heterogeneous media, motion | Recognized | Explicit unsupported diagnostics; no silent substitution | Unsupported |
| Unknown directives / malformed syntax | Source-located error | Stops without line-based recovery | Not attempted |
| Legacy v3 forms and hexadecimal numbers | Explicit diagnostic | Outside this profile | Unsupported |

Omitted descriptors and parameters remain omitted. Preflight does not install
demo defaults or imply that PBRT's implicit rendering defaults are implemented.
Supported descriptors retain authored settings for a later rendering translator.
Identifiers compare byte for byte; Unicode normalization of named resources
is outside this profile, so canonically equivalent spellings must use identical
bytes. `normal` follows the pinned implementation; `normal3` is also recognized
from the format documentation. Both raw-name and documented typed `Option`
forms are recognized, with option execution deferred.

Film output filenames are not input dependencies, texture parameter strings are
named references, and built-in spectrum names are not treated as filenames.

## rypath glTF extensions

Converted glTF bundles explicitly use `rypath-gltf-v1` materials, glTF-tagged
spectrum imagemaps and signed-frame PLY properties. The [glTF import profile](gltf-import.md)
defines their complete fields, sampler policy and restrictions. They are rypath
extensions rather than standard PBRT v4 syntax/semantics; ordinary PBRT material,
image and unmarked PLY interpretation remains unchanged. Structural acceptance
alone does not establish complete rendering or corpus compatibility.

## Declared descriptor parameters

Parameters outside these schemas are diagnosed rather than silently discarded.
Values use their PBRT types and cardinalities; optional settings stay omitted.
The last option descriptor is checked at `WorldBegin`, while camera coordinate
capture still occurs at each camera declaration.

| Descriptor | Checked settings |
| --- | --- |
| Perspective camera | `fov`, `lensradius`, `focaldistance`, `frameaspectratio`, four-value `screenwindow`, `shutteropen`, `shutterclose` |
| RGB film | `filename`, `xresolution`, `yresolution`, four-value `pixelbounds`/`cropwindow`, `diagonal`, `iso`, `whitebalance`, named `sensor`, `savefp16`, `maxcomponentvalue` |
| Box or triangle filter | `xradius`, `yradius` |
| Independent sampler | `pixelsamples`, `seed` |
| Sobol sampler | `pixelsamples`, `seed`, `randomization` (`none`, `permutedigits`, `fastowen`, or `owen`) |
| Path or volumetric path integrator | `maxdepth`, `regularize`, `lightsampler` |
| BVH | `maxnodeprims`, `splitmethod` |
| Diffuse material | `reflectance`, including a spectrum texture reference, scalar `displacement`, RGB image `normalmap`; named definitions require `string type` |
| Conductor material | Named/numeric/file `spectrum eta` and `k`; scalar/float-texture `roughness`, `uroughness`, `vroughness`; `remaproughness`, `displacement`, `normalmap`; named definitions require `string type`. RGB/blackbody/texture optical constants and the mutually exclusive reflectance shortcut are supported |
| Dielectric material | Scalar float or named/numeric/file spectrum `eta`; conductor roughness and surface-detail settings. Float eta takes precedence over spectral eta, including inherited values; ignored spectral files are not consumed. RGB/blackbody eta are supported |
| Extended surface materials | `coateddiffuse`, `coatedconductor`, `thindielectric`, `diffusetransmission`, nested `mix`, spectral tensor-v1 `measured`, and SCATFUN-v1 `fourier`; parameters, approximations and limits are in the [render profile](pbrt-rendering.md#extended-surface-materials) |
| Float constant texture | Optional scalar `float value` |
| Spectrum constant texture | Optional `value` of type `rgb`, `spectrum`, or `blackbody`, subject to the narrower render profile |
| Float/spectrum imagemap | Nonempty string `filename`; string `mapping`, `encoding`, `filter`, `wrap`; float `uscale`, `vscale`, `udelta`, `vdelta`, `scale`, `maxanisotropy`; bool `invert` |
| Infinite light | `L`, `scale`, `illuminance`, or square RGB PNG/JPEG/EXR/HDR `filename` |
| Distant light | Spectral `L`, nonnegative `scale`, finite `illuminance`, and distinct `point3 from`/`to` |
| Point light | Spectral `I`, nonnegative `scale`, finite `power`, and `point3 from` |
| Spot light | Point fields plus `point3 to`, `coneangle` in (0,180], and `conedeltaangle` in [0,coneangle]; distinct effective endpoints |
| Goniometric light | Spectral `I`, nonnegative `scale`, finite `power`, and required nonempty square gray/RGB/RGBA PNG/JPEG/EXR/HDR `filename`; declaration origin supplies position |
| Projection light | Required RGB/RGBA `filename`, nonnegative `scale`, finite `power`, and `fov` strictly between 0 and 180 degrees |
| Diffuse area light | `L`, `scale`, `twosided`, `power`, PNG/JPEG/EXR/HDR `filename` mutually exclusive with `L` |
| Triangle mesh | `P`, `indices`, per-vertex `N`, `S`, and `uv`, scalar/texture `alpha`; exactly three vertices may omit indices |

Triangle normals, tangents, and UVs are narrowed into owned indexed arrays. Face
indices remain unsupported. Triangle `vector3 S`, alpha, material bump/normal maps,
and infinite-light images follow the core rendering profile. Portals remain explicit
unsupported features. RGB reflectance is checked against PBRT's [0,1] rule;
sampled spectral values remain authored, without the demo's reflectance limits.

## Homogeneous medium structure

`MakeNamedMedium "name" "string type" "homogeneous"` retains the effective
`sigma_a`, `sigma_s`, `Le`, `scale`, `Lescale`, `g` and `preset` parameters, including
`Attribute "medium"` inheritance. Coefficient and emission inputs accept typed
RGB, blackbody, numeric spectra and built-in/file spectra; texture references
are outside this profile. Supported spectrum files are decoded with the existing
entry-relative budgets. Other medium types remain recognized and unsupported.

`MediumInterface "inside" "outside"` is valid before and after `WorldBegin`.
A single name binds both sides; an empty name means vacuum. A camera captures
the outside name when its `Camera` directive is processed. Shapes capture both
names, which follow attribute/object scope restoration and survive instancing.
Named medium definitions are global and references resolve after parsing,
including forward references and Includes. Empty/duplicate definitions and
undefined names are errors. `WorldBegin` preserves medium state while resetting
transforms. Structural success does not establish volumetric rendering support.

Inline `Material "interface"`, `Material "none"` and `Material ""`, and named
`interface`/`none` materials, describe null surface boundaries rather than black
BSDFs. Rendering rejects null mix children, null area emitters and nonopaque
alpha on null or medium-transition boundaries. The [render profile](pbrt-rendering.md#homogeneous-media)
defines preset precedence, spectral preparation and transport limitations.

## Constant texture definitions

`Texture "name" "float" "constant"` and `Texture "name" "spectrum" "constant"`
retain their definition and effective `value`, including values inherited from
`Attribute "texture"`. Float and spectrum names occupy separate global namespaces.
Empty names and duplicate names within a namespace are errors. Inline and named
diffuse materials resolve `texture reflectance` in the spectrum namespace after
parsing, including forward references and definitions in closed scopes. An empty
or missing reference, or a reference defined only in the float namespace, is an
error. Omitted constant `value` stays omitted until translation, where it defaults
to one.

The last compatible explicit parameter takes precedence over inherited defaults.
rypath accepts mixed compatible explicit types for the same field and selects
the last one. The pinned PBRT `ReportUnused` only recognizes shadowing within the
same type and name, so it can reject a shadowed explicit value of a different
type. This acceptance extension covers parameter selection in preflight. See
the [pinned parameter dictionary](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/paramdict.cpp).

<a id="png-imagemap-definitions"></a>

## Image map definitions

Named float and spectrum imagemaps use their respective typed namespaces and
follow the same parameter precedence rules. Filtering accepts `point`,
`bilinear` (the default), `trilinear`, and `ewa`/`EWA`. EWA supports finite
`maxanisotropy` from 1 through 16, defaulting to 8. Values outside that interval
are source-located unsupported-feature diagnostics, including unused definitions.
The other filters retain their inert finite `maxanisotropy` parameter.

Supported static PNG inputs are noninterlaced or Adam7-interlaced grayscale at 1, 2, 4, 8, or 16 bits;
grayscale with alpha, RGB, and RGBA at 8 or 16 bits; and indexed color at
1, 2, 4, or 8 bits.
Grayscale replicates into RGB; indexed inputs expand their palette to RGB8.
True RGBA alpha is retained for scalar filtering when nonopaque; grayscale-alpha,
palette transparency, and `tRNS` are ignored without compositing. RGB reflectance
never composites alpha. Metadata does not determine the color transform.
Animated PNGs are unsupported. JPEG accepts bounded 8-bit baseline/progressive
gray, RGB, or YCbCr input and rejects CMYK/YCCK and unsupported coding modes. A missing, corrupt, or over-limit supported input is an error.
A dependency is marked visited after successful decoding.
The decoder provenance and its byte-stream limits are recorded in
[LodePNG](../third_party/lodepng/README.md).

EXR accepts single-part flat scanline or one-level tiled images, including
nonzero data-window origins and partial edge tiles. The data window becomes
the texture extent; the display window does not allocate additional pixels.
Channels must be exactly unit-sampled Y, RGB or RGBA with one consistent HALF
or FLOAT type. Y replicates into RGB. Finite signed and above-one samples remain
linear; HALF widens exactly, and FLOAT remains binary32. Missing chromaticities
means sRGB/D65; authored chromaticities must match the pinned PBRT relative
`<1e-3` check. No color-space conversion or alpha compositing is performed.
True RGBA alpha follows the existing scalar consumer policy.

Compiled classic EXR compression includes NONE, RLE, ZIPS, ZIP, PIZ, PXR24, B44,
B44A, DWAA and DWAB. Multipart, deep, mip/rip levels, UINT or mixed channel
types, layered/extra/partial channels, subsampling, other chromaticities,
HTJ2K, LJ2K and ZSTD are explicitly unsupported. Nonfinite decoded values,
malformed chunks and resource exhaustion are errors rather than compatibility
fallbacks. Damaged chunk tables are not reconstructed.

Radiance `.hdr` accepts RGBE `#?RADIANCE`/`#?RGBE`,
`FORMAT=32-bit_rle_rgbe`, flat or modern per-channel RLE scanlines, and
`-Y height +X width` orientation. RGBE conversion is linear binary32, using
`mantissa * 2^(exponent - 136)` and black for exponent zero. XYZE, legacy
RGBE repeat-marker RLE, mixed flat/RLE scanlines, other orientations,
nonidentity exposure/color correction/pixel aspect and non-sRGB primaries
are unsupported. Unknown header assignments are unsupported; malformed or
truncated packets and trailing bytes are errors. Header text is never executed.
The [HDR record](hdr-image-validation.md) lists the exact limits and evidence.

Sixteen-bit grayscale-alpha and grayscale with `tRNS` are explicit rypath
extensions: the pinned PBRT decoder rejects conversion of those inputs. Owned
fixtures validate their channel and alpha-discard semantics; reference rendering
uses plain grayscale, RGB, and RGBA instead. See the
[16-bit validation record](image16-texture-validation.md).

Indexed inputs require a valid palette and reject every pixel index outside its
actual entry count, including unused texture declarations. Row-padding bits are
not pixels. Decoded palette colors use the existing eight-bit transfer and mip
path; transparent entries preserve their authored RGB. See
[indexed PNG validation](indexed-texture-validation.md).

Packed grayscale samples expand exactly to RGB8 using `sample * 255 / (2^depth - 1)`.
Transparency keys do not change the expanded samples. The existing eight-bit
transfer and mip path applies; see [grayscale validation](grayscale-texture-validation.md).

Asset paths in included files resolve from the entry directory. Distinct
images and PLY inputs share the file count and expanded byte budget with scene
files. One successful decode supplies all imagemaps of the same resolved path.
Mip variants share storage for the same path, encoding, and wrap mode, including
textures with different filtering modes. Aggregate
retained accounting includes decoded RGB/alpha bytes, packed mip texels, level
records, and custom gamma tables. Native views for normal maps and lights keep
the original dimensions and share storage when encoding and wrap agree.
PNG encoding may be sRGB, linear, or finite positive gamma. JPEG follows the
pinned fallback's fixed sRGB decode even when another encoding is authored.
EXR/HDR always use linear encoding; a valid authored encoding is inert for these
floating formats, following pinned PBRT. HALF EXR mips retain binary16 storage;
FLOAT EXR and RGBE use binary32 storage with four 32-bit words per RGB(A) texel.
Decoded floating RGB/alpha consumes 12/16 bytes per pixel, including widened HALF.
Image-specific resource limits are listed below.
Sixteen-bit sources retain six bytes per pixel and eight bytes per packed RGB
mip texel. The 4,096-pixel axis ceiling does not override byte limits: a full
4,096×4,096 RGB16 source and pyramid exceed the combined 256 MiB limit.

## Spectrum files

One bare or bracketed filename is supported in `spectrum reflectance` on diffuse
materials, `spectrum value` on spectrum constant textures, and `spectrum L` on
infinite and diffuse area lights. Homogeneous `sigma_a`, `sigma_s` and `Le` use
the same checked-file ownership. Exact built-in names take precedence over files.
Effective parameter selection, light defaults, and scope restoration are unchanged;
shadowed filenames do not become dependencies. Other file-spectrum consumers remain
unsupported. Unused supported material, medium and texture definitions are checked too.

Files contain at least two wavelength/value pairs in plain text. Decimal and
exponent forms, ASCII whitespace, and `#` comments are accepted. Comments may
immediately follow a number, and the final number need not have a trailing newline.
Every token must be a complete decimal number. Both columns narrow directly to
finite binary32; wavelengths must strictly increase after narrowing. Representable
subnormals are accepted, while nonzero tokens rounding to zero are errors.
Finite signed values remain checked data. Rendering separately requires
nonnegative emission and a positive finite CIE-Y normalization; diffuse reflection
clamps the evaluated curve to [0,1] and accepts zero curves.

Paths use the entry directory, with no current-directory fallback. Successful and
failed decode attempts are cached by the existing resolved dependency identity.
Only successful decoding marks a dependency visited. Descriptors share immutable
knots; translation never rereads the source. Diagnostics identify the authored
declaration and the file position. Malformed multi-file parameter lists and
ambiguous post-Include scans inventory paths without decoding them.

Spectrum files share the PLY/image file-count and expanded-byte budgets, and
the PLY decoded-scalar budget. The existing per-file, token, and list limits also
apply. Work already performed by a failed attempt is charged without refund. Light
translation and compiled reflectance are separately bounded by the spectral value
budget. Light, material, and texture descriptors share the same successful decode.

The stricter token parser, EOF handling, adjacent comments, and two-pair minimum
are documented differences from the pinned PBRT reader. See the
[file-spectrum validation record](file-spectrum-validation.md).

## PLY meshes

`plymesh` requires one nonempty effective `string filename`. The decoder supports
PLY 1.0 ASCII, binary little-endian and binary big-endian inputs, standard 8/16/32-bit
signed/unsigned integers, float32/float64, and their conventional aliases. Property
order is arbitrary. Vertex `x/y/z` and `face.vertex_indices` are required. Triangles
and cyclic quads are retained; quads use tensor corner order `(0,1,3,2)` and are
not triangulated. Larger well-formed faces report unsupported; invalid indices,
malformed data, nonfinite values and resource exhaustion are errors. ASCII
numbers use decimal syntax; nonzero values underflowing their declared scalar
type to zero are rejected consistently across standard libraries. PBRT/RPly may
round those values to zero instead.

Normals require all of `nx/ny/nz`. UV selection follows the first complete pair
from `u/v`, `s/t`, `texture_u/texture_v`, `texture_s/texture_t`. Partial recognized
attribute groups without a complete selection are errors. Other properties,
including `face_indices`, are consumed under the same work limits and remain
inert in this profile. This is not general PLY metadata support.
Alpha resolves a texture parameter before the float parameter, with an empty
texture name falling back to float. Active displacement requires UVs and a
positive finite edge length; it refines and displaces an owned geometry copy.
Empty displacement is inert, and finite `edgelength` is inert without displacement.
See the [consumer contract](pbrt-rendering.md#core-texture-graphs-and-consumers)
for emitting-alpha certification and displacement bounds.

Paths use the entry directory even in nested Includes. Unique decoded assets are
cached by resolved path components for this preflight only (redundant separators
are equivalent; dot and dot-dot components are preserved); each authored shape keeps
its own bindings and is charged for its geometry copy. Only successfully decoded,
validated assets become visited dependencies. Analysis after an unvisited Include
continues to inventory explicit paths without decoding them.

PLY bytes and unique decode attempts share the scene expansion budgets. Header
bytes are capped at 256 KiB, header lines at the directive limit, tokens at the
token-byte limit, and list lengths at the list-value limit. Declared rows and
aggregate decoded scalar work (including ignored data) are bounded by the total
value limit. These checks precede allocation and apply to failed attempts as well.
The translated-geometry budget separately counts copied positions, attributes,
triangle indices and patch indices. Diagnostics name the PBRT source plus the
asset's byte, header line or element row.

## State and numerical behavior

Transforms operate on the right of the current matrix. `Transform` replaces it;
`ConcatTransform` composes it. `LookAt` follows PBRT's camera convention rather
than the demo camera's convention. Camera capture stores the inverse of PBRT’s authored camera-from-world matrix
as camera-to-world; the named camera coordinate system uses that same inverse.
`WorldBegin` resets transforms and the active-transform selection and establishes
the world coordinate system. Other graphics state remains inherited.

Attribute scopes restore active graphics state. Named material, medium, object, texture,
and coordinate-system definitions are global; they survive a scope ending.
`NamedMaterial` and `ObjectInstance` bindings are resolved after parsing,
including forward references. Dependencies inside deferred material factories
are inventoried but do not establish their construction-order compatibility. Duplicate material/object definitions are errors. Object
members retain definition-time transforms, materials, medium interfaces, and orientation; instance
placement composes outside each member transform. Nested object definitions and
instances inside object definitions are rejected.

`AreaLightSource "diffuse"` captures effective light defaults and color provenance
in an immutable binding for subsequent shapes; attribute scopes restore it and
`AreaLightSource ""` clears it. Shape transforms, orientation, and material remain
independent. Emitting object-definition members are outside this profile, including
bindings inherited into a definition. An area declaration active at an ordinary
`ObjectInstance` does not change its members. A declaration without shapes emits
nothing. Positive `power` is structurally recognized but unsupported by the render
translator; structural success alone is not rendering compatibility.

The reader preserves authored spectral numbers and source provenance. It does
not map spectra to the renderer's eight knots or impose demo reflectance limits.
Checked geometry narrows to float positions/transforms and 32-bit indices.
Overflow, rounding-induced zero-area geometry, and singular transforms receive
geometry diagnostics separate from syntax errors. A fixed-size exact dyadic determinant predicate rejects singular binary64
linear matrices before inversion and composition, including cancellation cases.
It imposes no arbitrary determinant threshold. The subsequent inverse is still
computed in binary64; these checks do not provide a general forward-error bound
or prove world-space numerical conditioning or rendering equivalence.

## Files, incomplete analysis, and limits

Every relative include and input asset uses the supplied entry file's directory,
even inside nested includes. There is no current-working-directory fallback on
an entry-relative miss. Canonical identity is used only for active-cycle checks;
legitimate repeated includes expand again and count against limits. Includes
share state and may span attribute scopes. If an unvisited include leaves the
parent at an ambiguous operand boundary, that suffix is withheld rather than
guessing its directive arity. Unvisited imports/compressed files
and unchecked external assets make dependency completeness explicit.

Understood unsupported directives can be consumed and followed by further
analysis. Malformed syntax or unknown directive arity stops parsing. Deferred
state-changing constructs cannot be treated as absent from a complete result.
The diagnostic limit also marks analysis incomplete.

| Resource | Maximum |
| --- | --- |
| Bytes per file | 16 MiB |
| Bytes across repeated expansions | 64 MiB |
| Active files | 32 |
| File expansions, including entry | 256 |
| Active scopes | 256 |
| Token bytes | 64 KiB |
| Values per list | 1,000,000 |
| Total authored values | 2,000,000 |
| Directives | 100,000 |
| Diagnostics | 100 |
| Source or mip image axis | 4,096 pixels |
| Retained decoded RGB/alpha, mip/native views and transfer tables per scene | 256 MiB |
| Scratch per decode or mip build | 256 MiB |
| HDR header / individual header line | 64 KiB / 4 KiB |
| Individual PNG decoder allocation | 128 MiB |
| DEFLATE blocks per PNG | 65,536 |
| JPEG scans / cumulative DCT blocks | 256 / 2²² |
| Texture graph depth / worst-case visits | 64 / 4,096 |
| PLY refinement depth / texture evaluation work | 64 / 2²⁴ |

Tests inject smaller limits to exercise every boundary cheaply. Translation also
bounds repeated effective-parameter checks and expanded geometry against the
total-value budget. External assets are deduplicated by kind and resolved path,
preserving the first source location; repeated Include edges remain separate.
Cumulative resolved external-path bytes are bounded by the 64 MiB expansion
budget. Interpretation stops when the diagnostic budget is exhausted. These
checks prevent small repeated definitions from amplifying memory or work. The reader does
not execute scene-provided commands or create output paths.

## Validation scope

Portable CTests cover lexical and numeric boundaries, source/include locations,
parameter precedence and schema errors, transforms/camera capture, scoped and
global definitions, object composition, dependencies, limits, and narrowed
geometry. The unmodified [Cornell fixture manifest](../tests/fixtures/pbrt/README.md)
records the exact selected external coverage and expected inventory.
A matching upstream `--format` binary was unavailable for preflight PR #5.
The subsequent [rendering validation](pbrt-render-validation.md) builds the pinned
CPU reference for selected numerical comparisons; no large scene corpus is
downloaded. See [status](status.md) for measured checks.
