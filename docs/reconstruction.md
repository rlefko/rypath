# Frame reconstruction

Reconstruction consumes a completed GPU frame without changing its spectral XYZ
buffer, sample count, estimator, or history. `ReconstructionMode::raw_rgb` converts
that frame into separate unexposed linear RGB presentation storage.
`ReconstructionMode::metalfx` additionally generates deterministic guides and uses
MetalFX temporal denoising, at native resolution or an explicitly larger output
resolution. `ReconstructionMode::temporal` is a native-resolution, signed-float32
fallback with temporal accumulation and edge-aware spatial filtering. Every
reconstructed image remains separate from the raw Monte Carlo estimate.

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
the selected temporal filter in a second command. Invalid guides never enter it.
The second successful completion publishes temporal history. Failures invalidate
subsequent history eligibility. Frames reset on first use, explicit reset, epoch changes, or
lens radius/focal-distance or explicit camera-medium changes. Smooth camera
movement uses the previous successful camera. Submission order defines temporal
order; absolute render sample indices need not increase. Raw RGB has no temporal history.

Explicit capture copies the separate RGBA16F or RGBA32F output to CPU float RGB. Native
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
exposure. Raw-RGB and MetalFX modes clamp negative RGB and values above 65,504
in their half-float presentation input. The signed temporal mode preserves both
in float32. Counters report pixels with negative channels or channels above the
half-float limit, including when no clamp is applied. Nonfinite conversion fails.
No radiance clamp is added to raw rendering or Area/MCMC weights.

## Signed temporal fallback

Clamping fresh spectral samples before filtering changes their expectation.
An initial native 1080p demo capture has negative RGB in 2,020,236 of 2,073,600
pixels (97.43%). Independent float64 reductions give raw signed means
(.781046, .581514, .524913), versus (1.046654, .659763, .596325) after the input
clamp: increases of 34.01%, 13.46% and 13.60%. These are one-frame spatial
statistics, not a converged reference or an explanation of every MetalFX artifact.
The same interactive check shows persistent noisy sky and a washed-out interior.
Apple's linear-color contract does not establish signed spectral RGB or arbitrary
XYZ color-basis support. The MetalFX path therefore remains a separately labeled,
unqualified reconstruction option.

The fallback filters combined signed linear RGB in float32. It adapts temporal
moments and hierarchical filtering from
[SVGF](https://research.nvidia.com/labs/rtr/publication/schied2017spatiotemporal/)
and [edge-aware à-trous filtering](https://cg.ivd.kit.edu/publications/pubhanika/2010_atrous.pdf).
It does not reproduce SVGF's separate direct/indirect illumination or albedo
demodulation. RGB moments account for chromatic wavelength noise; they are filter
noise proxies under correlated Area reuse, not confidence estimates.

Temporal lookup uses current-to-previous motion without clamping offscreen
coordinates. Stored instance, primitive, primitive kind and resolved material
must match. Expected **previous-camera** depth and world normal reject mismatched
surfaces. Sky uses its own identity and rotation-only motion. Primitive matching
is deliberately conservative at triangle boundaries. Disoccluded pixels start
fresh. Defocus, media, transmission and mixed-material fallbacks trigger a
whole-frame temporal reset on camera changes; a stationary camera can still
accumulate same-pixel estimates. All mixes are conservative reset candidates
because their wavelength-dependent choice can differ from the fixed guide choice.
The existing guide approximation counts remain visible.

With accepted history length n, the mean update uses alpha=1/min(n+1,64) by
default. Centered per-channel variance uses
`v' = (1-alpha) * (v + alpha * (sample-old_mean)^2)`. After the cap this is an
exponential filter, not indefinitely growing accumulation. A one-frame history
limit uses exact replacement without subtracting the previous value. Unrepresentable
intermediate arithmetic fails visibly instead of clipping HDR samples. A cold neighborhood
estimate supplies spatial noise guidance while temporal history is short.
Five à-trous passes use steps 1, 2, 4, 8 and 16 with normalized nonnegative
geometry, albedo and chromatic-noise weights. The center remains eligible.
Temporal history stores the pre-spatial result, preventing recurrent spatial blur.
The API allows zero spatial passes for the independent temporal recurrence oracle.

The fallback remains an approximate reconstruction: reprojection, finite history
and data-dependent spatial weights can blur or bias output. Signed captures
retain negative values; only the separate display consumer maps them to its
nonnegative gamut. Correctness tests and reference-image quality qualification
are separate requirements.

## Headless measurement and capture

`rypath-frame-benchmark --reconstruction metalfx` measures native-resolution
reconstruction by default. `--output-width` and `--output-height` select explicit
upscaling; `--reconstruction raw-rgb` measures the color-conversion control.
`--reconstruction temporal` selects the signed fallback and requires native
output dimensions.
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

The signed fallback implementation `a6705bd` adds six focused reconstruction checks
on M4 Max with Metal API validation. Debug, Release and ASan/UBSan pass in
93.07 s, 55.45 s and 56.14 s respectively. These are test
durations, not frame-performance measurements. Independent double-precision
oracles cover 137 temporal updates across the 64-frame cap and all five spatial
passes, signed/HDR constants, history-limit-one cancellation, reprojection and
disocclusion, surface identity, variance overflow and recovery. Integration checks
cover signed capture, raw preservation, retained lifetimes, camera/media/mixed
material resets and sample indices above 2^32. Portable camera/CLI tests and
focused LLVM 18 analysis also pass. Image-quality qualification remains separate.

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
