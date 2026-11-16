# Pavilion transport and compatibility

This slice adds native analytic surfaces, four-wavelength vacuum transport,
bidirectional path tracing, finite RGB-film clamping, and bounded large-scene
preparation. Its external acceptance targets are the unchanged Barcelona Pavilion
day and night entry points. This is selected-scene coverage, not complete PBRT
parity. The existing finite-layer and measured-material limitations still apply.

## Model and estimator

`Integrator "bdpt"` uses camera and light subpaths, all eligible connection
strategies including camera splats, and balance-heuristic MIS. Directional PDFs
convert to endpoint area measure; delta events exclude unavailable strategies.
Radiance and importance scattering use their respective refraction conventions.
Camera sampling supports the existing perspective pinhole and uniform-disk thin
lens. Maximum depth is 0–64; camera/light capacities are depth+2/depth+1.
There is no roulette in this finite-depth packet implementation.

Four visible wavelengths share one geometry path. Dispersive transmission
terminates secondary wavelengths with the corresponding hero PDF adjustment.
The inverse CDF projects finite roundoff at valid endpoints onto its exact
360–830 nm support before evaluating the PDF; invalid random inputs still fail.
Coated materials share their internal walk across wavelengths; the sampled walk
weight remains separate from the approximate directional PDF used for MIS.
That existing finite stochastic coating model is an approximation.

BDPT selects uniformly from area primitives, positional lights, distant lights,
and one environment slot (including a black environment). Area samples use
object-area sampling and the world-space affine Jacobian. These proposals differ
from PBRT's light selection and directional shape sampling. Unclamped matched
models can be compared in expectation; nonlinear finite clamping can make means
depend on the proposal distribution.

The fixed QMC allocation uses 920 dimensions: 16 header dimensions, 65 camera
blocks of four, 64 light blocks of four, 65 direct-light blocks of four, and 64
camera-connection blocks of two. Material-local walks use keyed Philox streams.
Native storage is capped at 256 MiB; lane count shrinks with depth. Capacity or
arithmetic failures reject the batch rather than dropping paths or splats.
Packet pipeline specialization scans every scene material, including mix children,
and omits unused thin-sheet, diffuse-transmission, measured, Fourier and coated
families. Scalar entry points and full-family scattering probes keep their
existing dispatch behavior. Cold Metal pipeline creation remains a separate
startup cost that must be included in end-to-end timing.

Packet transport is selected by BDPT or a finite `Film.maxcomponentvalue`.
`path` and `volpath` then use the packet camera-path estimator with direct-light
MIS. Packet transport currently requires vacuum at the camera, every placed
medium interface, and light endpoints. Named zero-coefficient, zero-emission
media canonicalize to vacuum; active media receive source-located diagnostics.
Unclamped path/volpath retain the previous scalar transport and homogeneous media.

## Film and persistence

The CIE sensor evaluates `exposure * mean(L * CIE / wavelengthPDF)` before the
output RGB matrix. Finite `maxcomponentvalue` scales this sensor XYZ vector when
its largest component exceeds the limit. The summed camera contribution is
clamped once; each camera splat is clamped separately, before reconstruction.

Film retains compensated raw camera/splat XYZ and separate exposed, clamped
camera/splat planes. Camera samples normalize by their reconstruction weight;
splats normalize by samples per pixel and the actual filter integral. Splats do
not increment destination sample counts. Host reduction follows absolute sample,
source pixel, strategy, and destination order, without floating-point atomics.
`--raw-output PATH.exr` exports unexposed, unclamped FLOAT linear-sRGB output.
The ordinary output honors the authored exposure and clamp.

Checkpoint format 2 persists all planes, weights and counts. Identity, renderer,
sampling and accumulation contracts are 11/13/4/2. Finite-clamp exposure is part
of identity; unclamped exposure remains a presentation setting. Older contracts
are rejected explicitly. See [checkpointing](checkpointing.md).

## Analytic geometry

Sphere, disk and open cylinder use native Metal bounding-box primitives and
analytic intersection functions. Radius, height/z clipping, angular clipping,
disk inner radius, affine placement, instances, alpha, UVs, derivatives,
surface detail and area emission share the existing material/light machinery.
Rejected near roots still permit valid far roots. Visibility retains nearer
blockers on the sampled primitive.

For object area A and linear transform M, reciprocal area density is
`A * abs(det(M)) * length(inverse(transpose(M)) * n)` at the sampled point.
Positive-power normalization supports arbitrary affine disks and similarity
transforms for spheres/cylinders; other positive-power affine cases are diagnosed.
Ordinary radiance emission supports the full accepted affine domain.

Normals consistently use the outward inverse-transpose normal plus authored
`ReverseOrientation`. This deliberately resolves a reflected-transform
hit-versus-sampling discrepancy in the pinned PBRT analytic implementation.
Zero-area triangle faces in ordinary meshes retain their source index and emit
warnings. They have zero intersection and area-sampling measure; native triangle
slots use repeated vertices to avoid spurious collinear floating-point hits.
Positive-power zero-area emitters are rejected. Loop control meshes retain their
stricter manifold/nondegenerate requirements. STL, authored OBJ materials and
true displacement retain their existing zero-area rejection policies.

The analytic shader domain is deliberately bounded. Positive source lengths
and angular extents must be at least 2^-16; source lengths and the Frobenius
norms of the affine linear map and its inverse are at most 2^16. Differential
magnitudes must remain within the validated shader range. These conservative
limits leave headroom for binary32 quadratic, area and derivative arithmetic;
out-of-domain inputs receive diagnostics rather than invisible geometry.

## Corpus and resource profile

The [manifest](../tests/corpus/pbrt-pavilion.json) pins scene revision
`30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572`, both entry points, and all 159 dependency
files (327,674,842 bytes), with SHA-256 and Git blob hashes. Assets remain in an
ignored external cache. The acquisition helper verifies reusable files before
copying and verifies downloads before publication. Pavilion assets are by Hamza
Cheggour (CC BY 2.0); landscape assets by Jan-Walter Schliep, Burak Kahraman and
Timm Dapper / Laubwerk (CC BY 4.0); measured BSDF data are CC0. The manifest retains
per-file terms and the pinned upstream attribution document.

A fresh opt-in acquisition and authored day render can be reproduced with:

```sh
python3 scripts/fetch_pbrt_pavilion.py --fetch --max-download-bytes 327674842
build/release/rypath --render-pbrt \
  .cache/pbrt-pavilion/30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572/barcelona-pavilion/pavilion-day.pbrt \
  --resource-profile large --output build/pavilion-day.png \
  --raw-output build/pavilion-day-raw.exr
```

Use `pavilion-night.pbrt` and separate output names for the authored night scene.
Without `--fetch`, the helper reports its plan and writes no assets. Existing
verified assets can be reused through `--local-root` and `--local-file`.

`--resource-profile large` is explicit for both preflight and rendering:

| Budget | Large profile |
| --- | ---: |
| Single scene file / expanded text | 128 MiB / 1 GiB |
| Include expansions / decoded scalar values | 1,024 / 128,000,000 |
| Expanded triangle frames / shading-normal entries | 128,000,000 each |
| Maximum image axis | 8,192 |
| Retained image data / decoder scratch | 2 GiB / 1 GiB |
| PNG per-allocation ceiling | 1 GiB (standard: 128 MiB) |
| Estimated shading preparation | 16 GiB |

The preparation estimate includes owned/staged/uploaded shading data and packet
workspace. It excludes native acceleration structures, driver scratch and
pipelines, and is also checked against the device's recommended working set.
It is a rejection budget, not a promise of peak process memory. The default
`standard` profile preserves the previous limits.

| Authored target | Day | Night |
| --- | --- | --- |
| Resolution | 1600 × 850 | 1600 × 850 |
| Halton samples per pixel | 256 | 512 |
| Integrator | Omitted: volpath | bdpt |
| Maximum depth | Default 5 | Default 5 |
| Film clamp / ISO | 50 / 500 | 50 / 250 |
| Lens radius / focal distance | 0.0125 / 15 | 0.01 / 15 |

Preview variants change only resolution and sample count and must be labeled as
such. Successful acquisition and structural preflight alone do not establish
rendering conformance.

## Validation protocol

Portable tests cover profile boundaries, analytic parameterization, packet
spectra/film, transaction rollback, checkpoint round trips, identity and resource
accounting. Native tests cover intersection/orientation/alpha, spectral BSDF
agreement, scheduling, empty scenes, depth boundaries and analytic transport.

The opt-in [packet comparison helper](../scripts/compare_packet_transport.py)
uses pinned PBRT `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`, verified fixture
closures and 16 independent seeds per renderer at 8×8 and 4,096 spp. Full-image
and quadrant RGB means use a predeclared gate of five combined standard errors
plus `2e-5 * max(1, abs(reference))`. Cases include diffuse multi-bounce BDPT,
smooth/rough glass, four-interface delta chains, mirrors, constant/image
environments, mixed delta lights, lens/Gaussian splats and matched depth-zero
finite clamping. Logs, exact overrides and executable/library hashes are retained.

## Development validation record

Validation runs use an Apple M4 Max (Mac16,5, 128 GiB), macOS 26.6 / 25G5065a,
Xcode 26.6 / 17F113 and Metal 32023.883. Host tools are CMake 4.0.3, Ninja 1.13.2
and LLVM 18.1.8 for portable compilation and linting. Native tests execute on the
GPU; the separate Metal-disabled LLVM build is a macOS portability check, not a
Linux execution claim. The tested executable and metallib hashes are retained
with each comparison.

The implementation preceding material-family specialization passes all 200 Debug
and all 200 ASan/UBSan tests, plus all 54 native tests with Metal API validation.
The main packet comparison passes all 150 predeclared region/channel gates across
ten cases (160 renders per backend). Its retained run is
`.cache/packet-transport-comparison/packet-transport-1xc7hatm/`. This includes
multi-bounce diffuse transport, smooth/rough glass, four-interface delta paths,
mirrors, both environment types, mixed delta lights, thin-lens Gaussian splats,
and depth-zero finite clamping.

The material-specialized candidate preserves all 89 legacy reference PFMs
byte-for-byte against main `46b20f4`, and preserves the demo's raw sums,
compensations and counts. Its record is
`.cache/pavilion-validation/legacy-specialized/report.json`. The Python policy
and comparison-helper suite passes 204 tests. All four new native material-family
tests pass, including unbound mix children for thin dielectric, diffuse
transmission, measured and Fourier materials. The Metal-disabled LLVM build
passes all 148 tests. Final full-scene artifacts and build checks will be recorded
after their runs finish.

### Coated-material comparison boundary

The separate 16-seed coated-diffuse BDPT comparison does **not** establish PBRT
agreement: 10 of 15 gates pass. Full-image rypath RGB means are
`(0.130516, 0.199969, 0.276444)`, versus PBRT
`(0.131362, 0.201299, 0.278451)`, a roughly 0.6–0.7% deficit. The failed run is
retained at `.cache/packet-transport-comparison/packet-transport-zgmhib_2/`.
Neither seeds nor thresholds were adjusted to turn this result into a pass.

Controlled path-tracing variants use a finite clamp of `1e30` to select packet
transport without affecting these sample values. They pass all 15 gates:
rypath `(0.129904, 0.198921, 0.274925)`, PBRT
`(0.129951, 0.198970, 0.274910)`. The same pinned PBRT executable therefore also
produces significantly different path and BDPT means for this material.

Additional PBRT-only `simplepath` controls disable light sampling and either
sample the coating's joint walk or integrate its evaluated `f` with uniform
hemisphere directions. Their RGB means are respectively
`(0.129981, 0.198947, 0.275061)` and
`(0.138979, 0.214471, 0.297584)`. Increasing only the coating's internal depth
from 10 to 64 changes these means by at most `4.92e-5`, leaving the discrepancy.
These controls demonstrate sensitivity to the evaluation/sampling route in the
pinned stochastic layered model; they do not uniquely attribute the BDPT
comparison failure or prove the packet estimator unbiased. See retained inputs,
commands, all PFMs and gates under
`.cache/pavilion-validation/coated-simplepath-isolation/` and
`.cache/pavilion-validation/coated-path-isolation/`.

Increasing only the coating evaluation count from 1 to 16 also leaves the
matched BDPT discrepancy: 11 of 15 gates pass, with full-image means
`(0.130554, 0.200036, 0.276536)` versus PBRT
`(0.131321, 0.201227, 0.278353)`. This does not support treating the observed
offset as ordinary sample noise or as solely a depth-10 truncation effect.
The retained run is `.cache/pavilion-validation/coated-samples16-isolation/`.
The three matched coating variants are opt-in, reproducible helper cases; their
generated inputs match the retained diagnostic inputs exactly:

```sh
python3 scripts/compare_packet_transport.py \
  --rypath build/release/rypath_pbrt_reference \
  --case coated --case coated-path --case coated-samples16 \
  --output-dir .cache/packet-coating-comparisons
```

This command retains the failed BDPT gates and returns failure when they fail.

The existing fixed density-only stream deliberately differs from PBRT's
direction-hashed stochastic density; see the earlier
[layered validation](surface-material-validation.md). Coated BDPT remains an
explicit compatibility limitation pending a stronger attribution and matched
model validation. It is not counted among the ten passing packet cases.
