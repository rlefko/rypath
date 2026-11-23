# Frame reconstruction

Reconstruction consumes a completed GPU frame without changing its spectral XYZ
buffer, sample count, estimator, or history. `ReconstructionMode::raw_rgb` converts
that frame into separate unexposed linear RGB presentation storage.
`ReconstructionMode::metalfx` additionally generates deterministic guides and uses
MetalFX temporal denoising, at native resolution or an explicitly larger output
resolution. Neither output is a raw Monte Carlo estimate.

The native path uses the macOS 26 `MTLFXTemporalDenoisedScaler` API with ordinary
Metal command buffers. It does not require a Metal 4 command queue. Creation
checks SDK, OS, device, scale, and resource requirements. Unsupported MetalFX
requests return a diagnostic. There is no implicit reconstruction substitution.
The adapter's earlier synthetic M4 Max qualification establishes API execution;
it does not establish quality on path-traced images.

## Ordering and ownership

Build a stream from a completed `Frame`, then externally serialize submissions.
Source acquisition never waits for pending rendering or reads image pixels.
Pipelines belong to that source's immutable scene and intersection profile;
another session cannot reuse them. One reconstruction may be pending. Two output
slots remain reserved while results or GPU consumer leases retain them. Results
survive stream destruction; GPU consumers attach a lease before command commit.

The first command generates guides and converts XYZ to RGB. A completion callback
checks GPU status, error flags, and the exact converted-pixel count before encoding
MetalFX in a second command. Invalid guides never enter the denoiser. The second
successful completion publishes temporal history. Failures invalidate subsequent
history eligibility. Frames reset on first use, explicit reset, epoch changes, or
lens radius/focal-distance changes. Smooth camera movement uses the previous
successful camera. Submission order defines temporal order; absolute render
sample indices need not increase. Raw RGB has no temporal history.

Explicit capture copies the separate RGBA16F output to CPU float RGB. Native
presentation can retain its Metal texture without that copy. Timing includes all
reconstruction commands; it excludes explicit capture and is separate from raw
render timing. Texture dimensions are capped at 16,384 per axis for the target
M4 Max profile. The owned-resource budget includes textures and simultaneous
capture staging, but cannot account for MetalFX's private internal allocations.

## Guides and approximation policy

A center pinhole ray uses the same native triangles, analytic primitives, patches,
instances, alpha tests and surface-detail resources as transport. Null interfaces
and invisible emitter placements pass through. Mix visibility uses the existing
deterministic material choice at 550 nm. Invalid IDs, arithmetic, projections or
counter overflow fail reconstruction rather than becoming plausible guides.

Signed world-space normals, reversed infinite-far depth, and current-to-previous
pixel motion use the actual camera basis. Host matrices are rebased around each
camera origin. Background motion ignores translation. A previous projection
behind the camera receives a counted off-raster motion value. A current surface
before the guide near plane fails reconstruction; raw transport has no new near
cutoff. Per-path film/lens sampling is not a global projection jitter, so the
MetalFX jitter properties remain zero.

Albedo guides use sixteen fixed midpoint D65/CIE spectral samples, normalized to
RGB white. This deterministic quadrature is approximate, particularly for narrow
spectral features. Material mappings are also reconstruction proxies:

| Material | Guide policy |
| --- | --- |
| Diffuse | Integrated reflectance, zero specular |
| Conductor | Normal-incidence Fresnel specular |
| Dielectric / thin dielectric | Interface / thin-sheet reflection; transmission counted |
| Diffuse transmission | Total reflection-plus-transmission proxy |
| Coated surfaces | Interface plus attenuated substrate; layer approximation counted |
| glTF | Prepared base color and metallic-roughness proxy |
| Mix / measured / Fourier | Explicit white-diffuse neutral fallback, counted by family |

GGX roughness uses the maximum prepared alpha, discarding anisotropy. Wavelength-
varying roughness uses the maximum quadrature value. Mixed surface normals use
the geometric normal; bump evaluation uses 550 nm. Albedo and roughness range
clamps affect guides only and have counters. These mappings are our adaptation,
not a claim that Apple defines them as exact BSDF integrals.

A single surface cannot represent refracted layers or participating-medium
scattering correspondence. Media and transmission approximations are reported;
`defocus_approximation` identifies lens rendering with pinhole guides. Their raw
transport remains intact. These cases need measured reconstruction quality before
promotion to a qualified interactive profile.

XYZ conversion uses the renderer's model-specific matrix without authored
exposure. Negative RGB and values above 65,504 are clamped only in presentation
storage, with separate counters. Nonfinite conversion fails. No radiance clamp is
added to raw rendering or Area/MCMC weights.

## Headless measurement and capture

`rypath-frame-benchmark --reconstruction metalfx` measures native-resolution
reconstruction by default. `--output-width` and `--output-height` select explicit
upscaling; `--reconstruction raw-rgb` measures the color-conversion control.
The default `none` retains the raw-only benchmark. All modes retain independent
raw frame timing. `pipeline_summary` includes raw rendering, stream creation
when needed, guide generation and reconstruction completion in wall time. The
creation frame has no combined GPU timestamp because its contract probe is
untimed; it is never presented as a complete GPU measurement. Captures occur
outside measured frame intervals.

`--capture` writes the raw FLOAT EXR; `--reconstructed-capture` writes a separate
FLOAT EXR directly from the reconstruction's unexposed linear RGB, with output
kind, dimensions and source identity metadata. RGB is never passed through an
XYZ interpretation. Fresh destinations and sibling pending files are required.
Tone mapping and display exposure are separate consumers.

## Validation boundary

At implementation revision `c8b94b1`, five focused tests pass in each of Debug,
Release and ASan/UBSan with Metal API validation on M4 Max, macOS 26.6, Xcode 26.6
and SDK 26.5. Total test durations are 53.00 s, 37.89 s and 43.21 s respectively;
these are correctness-test durations, not frame-performance measurements.

The stream fixtures check an independent XYZ/matrix/half-conversion oracle,
unchanged raw XYZ, pending/busy behavior, two retained outputs, cross-queue GPU
leases surviving source/session/stream destruction, repeated sample indices,
epoch/reset/failure recovery, translation, rotation, lens changes, glass/media
approximation reporting, and native/upscaled output. Guide fixtures compare
analytic plane depth and normals, CPU camera motion, background rotation,
null/alpha-zero/invisible pass-through, transformed patches, spectral quadrature,
near-plane failure and counter overflow. They do not exhaust every material's
approximate guide mapping. Adapter tests also reject aliases and oversized
textures. Portable camera and benchmark CLI tests and focused LLVM 18 static
analysis pass.

A separate Release benchmark smoke executes six 128² frames each for raw RGB,
native MetalFX, 128²-to-256² MetalFX, and Area replay with one mutation plus
MetalFX. All 24 requested pipeline frames complete under API validation.
Independent OpenEXR 3.4.4/NumPy 2.4.5 decoding verifies finite FLOAT RGB, dimensions,
output-kind metadata and guide pixel accounting. The raw-RGB capture agrees with
an independent clamp/half-conversion oracle within two half ULPs and a float
arithmetic allowance. This validates direct-RGB publication, not denoising quality.

Reconstruction image quality, sustained complete-pipeline timing, and fallback
qualification remain separate gates. No denoised FPS or quality improvement is
claimed from these correctness tests.

Primary platform references: Apple's [MetalFX integration session](https://developer.apple.com/videos/play/wwdc2025/211/),
[temporal denoised scaler](https://developer.apple.com/documentation/metalfx/mtlfxtemporaldenoisedscaler),
[roughness guide](https://developer.apple.com/documentation/metalfx/mtlfxtemporaldenoisedscalerbase/roughnesstexture),
and [GPU feature limits](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf).
