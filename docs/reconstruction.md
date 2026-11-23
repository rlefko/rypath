# Frame reconstruction

The [diagnostic capture and replay tool](reconstruction-diagnostics.md) provides
identical-frame comparisons and explicit input/history/pass inspection before
changing reconstruction algorithms.

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

Color conversion and temporal completion/history telemetry aggregate exact integer
counts within each dispatched SIMD group before updating global counters. Partial
groups and out-of-image lanes contribute only their successful in-image pixels.
Failure flags remain immediate atomics, and guide subtotal additions retain overflow
checks. This changes execution only; pixel arithmetic, texture writes, temporal
recurrences and the raw estimator are unchanged. Surface-guide approximation and
pass-through counters retain their existing per-event accounting.

## Experimental MetalFX input repair

`submit(frame, ReconstructionSubmitOptions{.exposure = value})` supplies a
per-submission positive, finite display-exposure hint, rounded to R16F. Its
accepted range is [2^-24, 65504]. `ReconstructionInfo::exposure` reports the
encoded value. The existing `submit(frame, bool)` overload retains exposure 1.
Each job retains its own exposure texture through both completion callbacks.
Exposure never multiplies raw XYZ or output RGB; `preExposure` remains 1 because
input pixels are unexposed. This follows Apple's
[exposure-hint contract](https://developer.apple.com/videos/play/wwdc2025/211/).

`MetalFXGuidePolicy::primary_surface` remains the default.
`MetalFXGuidePolicy::appearance` is an explicit MetalFX-only experiment, following
Apple's [appearance-guide guidance](https://developer.apple.com/videos/play/wwdc2026/359/).
It evaluates view-direction Fresnel in the spectral guide quadrature. Smooth
conductor/glass guides trace reflected and transmitted geometry at 550 nm and
replace diffuse albedo, normal and roughness with resolved appearance proxies.
Glass combines the two surface appearances by Fresnel; its combined appearance is
stored in the diffuse guide with zero separate specular guide. The depth and
motion still describe the primary surface; this is a partial appearance-guide
adaptation, not complete reflected/refracted temporal correspondence. The signed
temporal mode always retains its primary guides and rejects this policy.

Each branch has at most four secondary surface events. Secondary smooth glass
continues transmission (or total-internal reflection), omitting secondary
reflection branches; secondary conductor throughput and glass continuation use
550 nm optics, so secondary spectral dispersion/tint is approximate. Terminal
surfaces use the existing 16-node spectral material quadrature. Unresolved
backgrounds, unsupported material families, exhausted bounds and media retain
primary proxies. `directional_approximation`, `appearance_replacement`,
`appearance_bounded` and `appearance_fallback` expose these decisions. This
policy does not modify transport or raw accumulation. The policy flag is part
of the exact guide-cache key; exposure is not a surface-guide dependency.

Optional specular-hit-distance and strength-mask inputs remain disabled. SDK
26.5's strength-mask header says 1 skips denoising, while WWDC26 describes 0 as
no denoising. Endpoint GPU tests are required before interpreting or enabling
that mask. No hit-distance convention has been qualified in this adapter.

The repair retains the explicitly clamped half-float MetalFX color input. It
does not establish arbitrary signed-RGB or XYZ support, repair spectral clipping,
or apply saturation/exposure compensation. New tests specify noise-free chroma,
small-feature contrast, directional mirror Fresnel, thin-glass blending,
policy/cache dependencies, invalid exposure and raw preservation. These tests
have not yet been built or executed on this revision; no new performance or
image-quality qualification is claimed.

## Exact surface-guide reuse

`ReconstructionConfig::cache_surface_guides` and benchmark `--guide-cache on|off`
select a stream-local surface-guide cache. The API default is **on** after the
[static and phased motion qualification](core-performance.md). The benchmark
defaults on for temporal/MetalFX and off for none/raw-RGB; explicit on/off wins
regardless of option order. Explicit on requires a surface-guide mode. The viewer
uses the API default for temporal/MetalFX. This changes execution only: fresh
XYZ-to-RGB conversion and all selected temporal/spatial passes still execute every
frame. Raw XYZ, sampling, reconstruction settings, and temporal recurrence are
unchanged.

The cache owns the existing single set of depth, motion, diffuse/specular albedo,
normal, roughness, previous depth and identity textures, plus temporal-only
camera-relative position, geometric normal and indexed continuity-group textures. Output slots and temporal
history banks remain independent. The explicit fieldwise key covers the resolved
full-film pinhole parameters bound by `bind_guides`, all texture parameters
(including sample scale and camera footprints), and the complete current/previous
projection configuration. Only transport sample indices are additionally
normalized. Session/profile authorization still executes on every submission;
source slot and buffer addresses do not participate. A cold static stream misses
on frames one and two, then hits once previous-camera validity is stable.

Misses invalidate before overwriting surfaces. Explicit reset, epoch, lens or
medium changes invalidate reuse; movement and previous-camera changes miss through
the key. Only successful completion of the entire reconstruction publishes a key
and logical surface-counter snapshot. Failures invalidate both cache and temporal
eligibility. Hits preserve logical surface counts while freshly computing color,
conversion counts and color-range diagnostics. `guide_cache_hit` and
`generated_surface_pixels` distinguish reused guides from executed surface work.

The matched-stream test compares all eleven temporal guide textures byte for byte
(the three temporal-only fields are empty for MetalFX), output,
logical counters, history decisions and raw preservation across static/cap,
translation/rotation holds, reset/epoch, lens, texture-camera, medium, session
authorization and recovery cases.
These tests require actual Metal execution; portable key and CLI checks alone do
not establish hardware correctness or a performance benefit.

## Signed temporal fallback

Clamping fresh spectral samples before filtering changes their expectation.
An initial native 1080p demo capture has negative RGB in 2,020,236 of 2,073,600
pixels (97.43%). Independent float64 reductions give raw signed means
(.781046, .581514, .524913), versus (1.046654, .659763, .596325) after the input
clamp: increases of 34.01%, 13.46% and 13.60%. These are one-frame spatial
statistics, not a converged reference or an explanation of every MetalFX artifact.
The same interactive check shows persistent noisy sky and a washed-out interior.
Apple's linear-color contract does not establish signed spectral RGB or arbitrary
XYZ color-basis support. The MetalFX path remains a separately labeled option;
the measured integration limits are discussed below.

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
remains deliberately conservative for temporal correspondence at triangle boundaries. Disoccluded pixels start
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
geometry, albedo and chromatic-noise weights. Spatial support separately permits
triangles within the same instance, resolved material and connected indexed
coplanar group. Groups follow manifold shared indexed edges, never position
welding; every group's plane and tolerance are anchored to its seed triangle,
preventing transitive bending. Duplicate/nonmanifold faces and disconnected or
coincident sheets remain separate. Camera-relative hit positions and full-float
geometric normals supply symmetric point-to-plane checks; shading normals and
albedo retain appearance edges. Curved patches and analytic primitives retain the
existing exact-primitive reversed-depth/shading-normal weights; planar joins apply
only to triangles. The optional `strict_spatial_segments` experiment (default
**false**) additionally checks intervening raster guide pixels for dilated taps.
Its cost requires measurement before adoption. Default endpoint support cannot
distinguish a visible hole within one connected coplanar group; subpixel gaps are
also limited by the deterministic center-ray guide representation.

Continuity metadata is prepared lazily only when temporal reconstruction is
requested, using
immutable resident geometry. Ordinary offline scene construction does no topology
preparation and retains no extra geometry copies. Raw-RGB and MetalFX modes do
not prepare continuity or allocate the three new spatial guide textures. Preparation preflights a
conservative peak allowance (768 bytes per resident triangle, 64 per resident
vertex and 64 KiB overhead) against the scene preparation limit, an 8 GiB cap and
the device working-set recommendation. The shared GPU group buffer is also
charged to reconstruction's owned-resource estimate. Allocation failures return
an explicit diagnostic.

The signed chromatic exponential retains its squared-distance form and original
absolute `1e-4` denominator floor. `SpatialVariancePolicy::paired` remains the
default, using `4 * (center_variance + neighbor_variance) + 1e-4` per channel.
The explicit `center` experiment uses only center variance; it can reject outward
halo tails while attenuating the uncertain bright center itself. The explicit
`conservative` experiment uses componentwise minimum center/neighbor variance,
requiring compatibility in both directions. These are our SVGF-inspired RGB
adaptations, not the paper's exact luminance weighting or a published quality claim.
The fixed floor limits exposure equivariance near zero variance.

The separately selected `variance_prefilter` option (default **false**) is accepted
only with the center policy, keeping policy ablations unambiguous. It applies a
separable `[1,2,1]` Gaussian times geometry/appearance weights to the 3x3 guidance
variance proxy. Incremental normalized means avoid overflowing a raw variance sum
when its normalized average is finite. Original temporal moments, propagated
spatial variance and pre-spatial temporal history remain independent.
The center remains eligible; signed radiance and history are never clipped.
Temporal history stores the pre-spatial result, preventing recurrent spatial blur.
The API allows zero spatial passes for the independent temporal recurrence oracle.

The fallback remains an approximate reconstruction: reprojection, finite history
and data-dependent spatial weights can blur or bias output. Signed captures
retain negative values; only the separate display consumer maps them to its
nonnegative gamut. Correctness tests and reference-image quality qualification
are separate requirements.

The current signed repair adds 129x129 impulse/sign/constant/HDR and exposure
fixtures and explicit paired/center/conservative signal-retention metrics,
oblique-plane triangulation invariance, disconnected close-layer and
strict-mode visible-gap rejection, curved-surface oracle comparisons, lazy
multi-mesh metadata alignment, finite HDR Gaussian guidance, plus portable
indexed-topology checks. Noise-free narrow HDR and transient-signal fixtures are
synthetic filter checks; they do not establish physical caustic/reflection quality. These are added
checks awaiting execution; historical results below do not validate this repair.

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

The initial SIMD counter-aggregation working tree based on `4b1cc65` passed five
focused Debug checks (`metal.reconstruction_raw`, `metal.reconstruction_temporal`,
`metal.reconstruction_signed`, `metal.reconstruction_guides`, and
`metal.temporal_denoiser`) with Metal API validation on M4 Max, macOS 26.6,
Xcode 26.6 and SDK 26.5. Its fixtures covered widths 1, 31, 32 and 33, padded
color dispatch, exact success/history totals, multi-pixel counter overflow,
nonfinite input and failure recovery. The portable camera test also passed.
This is historical correctness evidence.

Later integrated source `54fa952` passes 14 focused checks in each of Release and
ASan/UBSan with Metal API validation and no skips; see the
[integrated correctness record](core-performance.md#integrated-correctness-checks)
for the scope and test durations. The
[mechanical counter comparison](core-performance.md#reconstruction-counter-qualification)
establishes no meaningful performance gain, with no measured regression
approaching the declared 3% limit. It changes execution only, preserving the raw
estimator and reconstruction recurrences.

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

These correctness tests alone establish neither denoising quality nor rendered
FPS. The separate measured study below supplies bounded quality and timing
evidence.

## Measured pipeline snapshot

The [audited qualification](pipeline-qualification.md) at historical `0ef51ba`
compares both reconstruction modes on six static fixtures and signed temporal
reconstruction on five eligible motion fixtures. Independent/temporal has lower
post-cap physical MSE than Area-one/temporal in all six static fixtures, supporting
the viewer's independent/temporal default. Finite references, motion-anchor-only
truth and the mostly uniform coated fixture limit the conclusion.

The MetalFX findings describe this integration, not the framework generally.
Its primary pinhole/F0 guides omit reflected/transmitted surface replacement and
Fresnel blending; its input clips negative spectral RGB. Exact GGX roughness and
per-pixel stochastic sampling versus global jitter remain unresolved integration
questions. These limits remain separate from raw transport. The native timing
study misses the suite's 20–30 FPS target, including all four sustained controls.
See the qualification record for full tables, uncertainty and timing scopes.

Primary platform references: Apple's [MetalFX integration session](https://developer.apple.com/videos/play/wwdc2025/211/),
[temporal denoised scaler](https://developer.apple.com/documentation/metalfx/mtlfxtemporaldenoisedscaler),
[roughness guide](https://developer.apple.com/documentation/metalfx/mtlfxtemporaldenoisedscalerbase/roughnesstexture),
and [GPU feature limits](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf).
