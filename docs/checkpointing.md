# Resumable offline rendering

Both render commands accept optional checkpoint files:

```sh
./build/release/rypath --render-pbrt scene.pbrt --output image.exr \
  --checkpoint render.rycp --checkpoint-interval 60

./build/release/rypath --render-pbrt scene.pbrt --output image.exr \
  --resume render.rycp --batch-size 4
```

Keep the original scene and assets available. A checkpoint stores accumulation
and identity, not a second scene format. Any filename is accepted; `.rycp` is the
documented convention. `--resume` updates its input file unless `--checkpoint`
selects another destination. The image output argument remains required.
`--checkpoint-interval` takes positive integer seconds and requires either
checkpoint option. Without those options no checkpoint is written.

The default interval is 60 seconds. Saves occur before the first batch, at the
first committed batch after the interval expires, and at completion or graceful
cancellation. Saving is synchronous; there is no background snapshot race.
Ctrl-C or SIGTERM requests cancellation after the current complete batch. The
program saves the committed Film, reports its sample count, leaves the image
destination untouched, and returns 130 or 143. Without checkpointing, cancellation
still stops between batches but does not preserve state on disk. Cancellation
latency includes the whole current batch and checkpoint publication; it is not a
deadline. SIGKILL cannot produce a final save.

Reaching the requested sample count takes precedence over simultaneous
cancellation. Rendering and publication errors take precedence over cancellation
and return 1. A failed batch never contributes samples. A previous published
checkpoint remains usable; work after that checkpoint may have to be recomputed.
An already complete checkpoint can publish an image without acquiring a GPU.

## Identity and compatible continuation

Resume restores every raw binary64 XYZ sum, Neumaier compensation term and
64-bit count. No image readback, averaging or output conversion reconstructs this
state. The next sample index is the uniform committed count. Random draws remain
functions of pixel, seed, absolute sample and the existing sampler domains.
Cancellation does not selectively discard slow paths or change the estimator.

Identity is a versioned canonical SHA-256 stream over owned render data: geometry,
transforms, camera/filter, surfaces and tables, image pyramids and texture graphs,
lighting, media/interfaces, transport controls, dimensions, sampler, seed and
target sample count. Shared resources are hashed by content, never address or
object padding. Files are not reopened to construct identity. Changing an asset
must produce the same effective owned data to remain compatible; inactive fields
are conservatively included. Bitwise changes in translated data can reject a job
even when two scenes appear visually equivalent.

Static glTF identity includes material factors and image bindings, sampler policy,
native image data, signed tangent frames and material-derived emission bindings.
These fields participate in the versioned renderer/identity contract; adding
glTF support does not make older incompatible checkpoints resumable.

The original target sample count is fixed: it controls texture footprints.
Point-light positions and prepared spectral intensities participate in scene
identity. Spot positions, intensities, inverse direction maps and cone thresholds
also participate. Spot lighting advanced the identity schema to 4. Renderer
contract 5 changes bounded-medium numerical paths: null boundaries use a
ray-anchored conservative normal offset with an outgoing-side certificate, and
finite medium visibility retains its excluded endpoint across crossings instead
of subtracting projected displacement. The transport model, PDFs and random
streams are unchanged. Invalid arithmetic, uncertain side or endpoint checks and
the crossing budget still fail the entire batch. This policy does not promise
support for arbitrary scales or coincident shells.

Distant lighting introduced renderer contract 6 and identity schema 5.
Its ordered directions, all radiance bins and finite-visibility scene radius
participate in identity. Goniometric lighting introduced renderer 7 and identity
to 6: ordered positions, all spectral intensity bins, actual inverse direction
maps, image bindings and owned scalar map contents participate. Earlier
renderer/identity contracts are rejected for every scene.
The identity stream also includes the renderer contract. Sampling, accumulation
and file-format versions remained 1 at that revision. See the
[goniometric profile](pbrt-rendering.md#goniometric-lights) and
[validation record](goniometric-light-validation.md).

Projection lighting introduced renderer 8 and identity 7. Ordered positions,
actual inverse maps, image bindings/scales, projection coefficients and deep image
contents participate. Renderer 7 and identity 6 checkpoints are rejected, including
for scenes without projection lights. Sampling, accumulation and file-format
contracts remained 1 at that revision. See
[projection validation](projection-light-validation.md).

Renderer contract 9 introduced the exact-tangent emitter-hit PDF rule to triangles
as well as bilinear patches. An accepted hit with zero endpoint cosine receives
the BSDF technique's full weight; other invalid arithmetic still rejects the
batch. At that revision identity remained 7. Earlier renderer checkpoints are rejected,
including scenes without area lights. See [tangent emission validation](tangent-emission-validation.md).

Perspective depth of field advances renderer to 10, identity to 8 and sampling to
2; accumulation and file-format contracts remain 1. The optional lens record
hashes radius, focal distance and both captured camera-plane axes. Zero authored
radius omits that record, so changing its inert focal distance does not change
effective scene identity. Active lenses add the disjoint `lens-v1` sampling
extension; existing camera, wavelength and transport draws retain their assignments.
All earlier renderer, identity or sampling contracts are incompatible, including
checkpoints for pinhole scenes whose rendering sequence is unchanged. See
[depth-of-field validation](depth-of-field-validation.md).

Checkpointing does not add a PBRT sample override or allow extending a completed
job. Batch size, file paths, output precision and presentation exposure may change.
Restored Film uses the current validated exposure without modifying raw values.

Compatible builds and supported native Metal GPUs may continue a checkpoint when
the canonical identity and declared renderer, sampling, accumulation and identity
contracts match. Product version, compiler configuration, source fingerprint,
device and OS are provenance, not compatibility gates. Each nonempty contributing
session records a half-open sample range; exporting a completed file adds no GPU
contribution. EXR metadata also records checkpoint identity and provenance.

Exact raw equality with uninterrupted rendering is tested on the same binary and
device. Cross-build/device continuation preserves historical bits and sample
accounting, but future floating-point paths may differ. It is not a promise of
bitwise cross-device output. A dispatch checks the actual loaded Metal library's
contract words against the host; a missing or stale library fails with rebuild
guidance. Unknown checkpoint versions fail without a force-resume option.

Changes to transport models, numerical policy, sample mapping, canonical fields,
or accumulation semantics must review and bump the appropriate shared contract
in `checkpoint_contract.h`. Intentional incompatible changes reject old files;
there is no v1 migration. Compiler configuration must preserve the declared
precision policy, including existing shader `-fno-fast-math` settings.

## Format, limits and publication

Version 1 uses explicit little-endian integers and IEEE binary64 bit patterns.
The 44-byte header contains the eight-byte `RYPCHK\r\n` magic, five uint32
contract versions, and uint64 metadata/pixel-payload lengths. Metadata stores
render settings, Film model/exposure, 32-byte identity and bounded provenance.
Each pixel occupies 56 bytes: three sums, three corrections, then its uint64
count. A trailing 32-byte SHA-256 covers all preceding bytes. The
[NIST SHA-256 specification](https://csrc.nist.gov/pubs/fips/180-4/upd1/final)
defines the hash; it detects corruption and is not an authenticity signature.

Limits are 4 GiB of pixel payload, 1 MiB of metadata, 1,024 provenance segments
and 4,096 bytes per provenance string. Existing Film and sampler limits also
apply. The reader verifies bounded lengths and checksum before allocating Film,
then checks the bytes actually parsed again. Invalid versions, trailing data,
nonfinite accumulation, inconsistent counts, and impossible provenance ranges
fail. Zero samples require zero accumulation.

Publication writes a sibling temporary file, checks all writes, requests file
durability (`F_FULLFSYNC` on macOS, `fsync` on other POSIX systems), checks close,
renames atomically, and synchronizes the parent directory. This follows
[Apple's distinction between flushing and durable storage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html).
Pre-rename failures preserve the previous destination. A parent-directory failure
after rename reports that publication succeeded but durability is uncertain;
it does not falsely claim the old file survived. Guarantees depend on filesystem
and storage support; unsupported synchronization fails visibly.

Checkpoint/image and checkpoint/input aliases are rejected before publication,
including existing hard links and symlinks. Use one writer per checkpoint path;
concurrent independent render jobs targeting the same file are unsupported.
Temporary files from a hard process kill may remain beside a valid checkpoint;
they are never treated as resumable input automatically.

See [checkpoint validation](checkpoint-validation.md) for measured coverage and
remaining device-validation limits.

Positive diffuse area power advances renderer to 11 and identity to 9; sampling
remains 2, and accumulation/file-format remain 1. Canonical emission includes
binary64 power, with finite nonpositive authored values represented as zero.
Factors derive from final world geometry and image content already covered by
identity. Older renderer/identity checkpoints are rejected for every scene.
