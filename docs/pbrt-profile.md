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
| Perspective camera, RGB film, box/triangle filter, independent or Sobol sampler, path integrator, BVH | Retained | Declared parameter schemas checked | See the separate [render profile](pbrt-rendering.md) |
| Diffuse materials and named bindings | Retained | Last matching typed parameter wins; explicit values override inherited defaults | Not translated |
| Constant infinite-light descriptors | Retained | Descriptor and reference checks | No normalization or light sampling |
| Diffuse triangle area-light declarations | Retained | Scoped shape binding, defaults, reset, and descriptor checks | No normalization or light sampling |
| Color spaces, RGB, blackbody, sampled spectra, built-in spectrum names | Retained | Types and references checked | No resampling, clamping, or reconstruction |
| Inline trianglemesh and single-level object instances | Retained | Converted to checked float/index geometry | Not submitted to Metal |
| Plain-text Include | Retained and expanded | Shared scene state, entry-directory resolution | Not applicable |
| Import and gzip scenes | Recognized | Unvisited; dependency inventory incomplete | Unsupported |
| External meshes, image textures, file spectra | Recognized; discoverable paths reported | Content unchecked; missing inputs are errors | Unsupported |
| Textured area lights, emitting object members, other shapes/materials, media, motion | Recognized | Explicit unsupported diagnostics; no silent substitution | Unsupported |
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
| Path integrator | `maxdepth`, `regularize`, `lightsampler` |
| BVH | `maxnodeprims`, `splitmethod` |
| Diffuse material | `reflectance`; named definitions require `string type` |
| Constant infinite light | `L`, `scale`, `illuminance` |
| Diffuse area light | `L`, `scale`, `twosided`, `power`, empty `filename`; nonempty image filenames are inventoried and deferred |
| Triangle mesh | `P`, `indices`, per-vertex `N` and `uv`; exactly three vertices may omit indices |

Triangle normals and UVs are narrowed into owned indexed arrays. Tangents, face
indices, and alpha are retained and cardinality-checked but remain unsupported.
Material displacement/normal maps,
texture-valued reflectance, and infinite-light images/portals are also explicit
unsupported features. RGB reflectance is checked against PBRT's [0,1] rule;
sampled spectral values remain authored, without the demo's reflectance limits.

## State and numerical behavior

Transforms operate on the right of the current matrix. `Transform` replaces it;
`ConcatTransform` composes it. `LookAt` follows PBRT's camera convention rather
than the demo camera's convention. Camera capture stores the inverse of PBRT’s authored camera-from-world matrix
as camera-to-world; the named camera coordinate system uses that same inverse.
`WorldBegin` resets transforms and the active-transform selection and establishes
the world coordinate system. Other graphics state remains inherited.

Attribute scopes restore active graphics state. Named material, object, texture,
and coordinate-system definitions are global; they survive a scope ending.
`NamedMaterial` and `ObjectInstance` bindings are resolved after parsing,
including forward references. Dependencies inside deferred material factories
are inventoried but do not establish their construction-order compatibility. Duplicate material/object definitions are errors. Object
members retain definition-time transforms, materials, and orientation; instance
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
