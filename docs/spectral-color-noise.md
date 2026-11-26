# Spectral color noise

This change affects sampling and reuse covariance, not scene spectra, geometry,
materials, exposure, denoisers or path count. Each fresh path still carries four
wavelengths. Smooth dispersive transmission may retain only the hero, and coated
materials retain the documented compensated hero-only fallback.

The measured candidate remains experimental: Cornell improves, but Prism misses
RGB-error gates. Automatic reuse remains unchanged. See the measured results
below for the retained regressions and limits.

## Research and diagnosis

[PBRT, Film and Imaging](https://pbr-book.org/4ed/Cameras_and_Film/Film_and_Imaging)
and the [Weta spectral rendering course, section 4.2](https://www.wetafx.co.nz/assets/Uploads/PDFs/sig-course-2022.pdf)
motivate probability-stratified visible wavelength sampling. The previous hero
implementation sampled equal intervals in nanometers; its near-local wavelength
mutation moved at most about 1.84 nm. Reusing nearby histories can preserve
spectral correlations even when each individual estimator is correctly weighted.
The [ReSTIR PT enhanced paper](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/)
also motivates testing less-correlated spatial sources. These are hypotheses to
measure, not evidence of a discovered bias or a promised MSE improvement.

Other spectral approaches were considered. [Continuous MIS](https://iliyan.com/publications/ContinuousMIS/)
combines observer and emitter spectral importance; [spectral guiding](https://markvanderuit.nl/publications/2021-11-27-paper-spectral)
learns spectral importance. Both require additional proposals and validated
support for every light type. Current light-selection records retain scalar
power rather than complete aggregate spectra. RGB averages cannot silently
stand in for the nonlinear RGB-to-spectrum reconstruction of textured lights.
Neither technique is claimed implemented here. The Prism fixture uses SF11 glass
and white RGB emitters; its name does not indicate a fluorescent F11 illuminant.

## Estimator contracts

For uniform primary coordinate u, lane k uses H((u+k/4) mod 1), where H is the
visible inverse CDF. Each cyclic shift preserves the uniform measure. Thus the
four alternative heroes still have equal technique weights in u space, and
cumulative directional likelihoods retain the existing balance mixture. Sensor
conversion divides each lane by its own q(lambda), once. An extra ratio of
wavelength densities in the directional prefix would count the change of
measure twice. The fitted inverse CDF/PDF and binary32 endpoint projection have
finite precision, covered by independent quadrature and CPU/MSL checks.

`--area-mutation-policy spectral-refresh` selects a fixed half-and-half mixture:
replace the entire wavelength primary word uniformly, or perform the legacy
block mutation. Equality is a valid self proposal; there is no retry. Both
components are symmetric, including overlapping transitions. Existing actual
dyadic Metropolis acceptance and weighted mutation correction therefore apply
unchanged. One mutation still costs one replay, without a new light path budget.
See the [MCMC mutation paper](https://arxiv.org/abs/2211.00166).

`--area-stencil reciprocal16` draws two independent indices on the 128 integer
points of the Chebyshev-radius-16 perimeter. A dedicated Philox domain uses the
seed and full accepted submission serial. The five slots are center, d0, -d0,
d1, -d1. Out-of-bounds slots map to self; duplicates remain. Every pass uses the
same immutable frame offsets, with the opposite cache plane on reciprocal edges.
This preserves the source-target MIS construction while changing covariance.
Automatic policy selection remains legacy/adjacent pending measured adoption.

Renderer/sampling contracts are 29/11; Area contract is 6. Effective hero sampling
enters checkpoint identity. CLI reports distinguish requested and effective
policies, and Area frame records include the actual offsets. Dormant nonhero
wavelength choices do not alter scene identity.

## Validation protocol

Portable tests cover exact cyclic shifts over all 2^24 uniforms, independent
spectral quadrature, finite cyclic path enumeration, mutation symmetry and
weighted nonstationary expectations, reciprocity, borders and duplicate slots.
Native tests add CPU/MSL wavelength mapping, analytic cold Area0 sensor output,
dispersive transmission, early emission, full-width indices and queue scheduling.

The local study `spectral-color-repair-003` predeclares 336 raw screening jobs:
ten common arms across both scenes plus a Prism-only legacy packet control,
16 session seeds, 160x90, 64 frames. Cornell retains authored depth 65; legacy
packet supports at most 64, so that unavailable control is omitted without
changing the scene. The Prism packet comparison is descriptive and does not
affect adoption. Four independent scalar
4096-frame online-mean batches per scene provide references. Quality uses the
final single-frame signed RGB, chroma and low-frequency chroma MSE. Paired
bootstrap resamples entire session seeds and independent reference batches,
with a shared reference draw across arms; pixels are not independent replicates.
Selected settings then face 32 held-out seeds, 128 frames and 15 whole/quadrant
RGB mean safeguards. Existing failed Prism evidence remains recorded separately.

Adoption requires both scenes' low-frequency chroma ratio upper 95% bounds below
one, a geometric-mean point ratio at most 0.75, and RGB MSE upper bounds at most
1.05. Mean tolerances are the maximum of five combined standard errors, 2% of
pooled mean magnitude and 1e-5. Native 1080p complete signed-pipeline timing uses
three alternating baseline/candidate pairs, eight warm-up and 96 measured frames;
median and p95 paired point ratios must not exceed 1.05. These gates are set
before observing outcomes. No candidate passed all quality gates; both scenes
passed the relative timing gate.

Final Cornell and Prism sheets use native 1920x1080, seed 51001, 128 static
submissions and frame 127. PT, Area0 and Area1 raw panels accompany three
reconstructions of the identical Area1 stream: temporal-only, signed spatial
plus temporal, and native MetalFX. Presentation uses fixed exposure 1,
Reinhard and sRGB; signed FLOAT source images remain unchanged.

## Measured results

The measured implementation is `2fa32662df3a3fc874f5a8887f386a707d0322c0`,
with Release source fingerprint
`0b30faca98ce226238f454eb598f20ae53e9e2c0f4e61bff410faa8b65748763`.
The hardware is an Apple M4 Max MacBook Pro (40 GPU cores, 128 GB), macOS
26.6 (25G5065a), Xcode 26.6 (17F113). Numerical captures use Metal API validation;
timing disables it. This evidence does not establish the 20–30 FPS suite target.

No screened candidate passed all adoption gates. The predeclared fallback,
visible wavelengths with spectral refresh and reciprocal16 reuse, therefore
remains **diagnostic only**. Automatic mutation/stencil selection stays
legacy/adjacent. The visible wavelength sampler is the default only inside
explicit hero transport; automatic transport still selects scalar/packet.

Ratios below compare Area1 against uniform-hero, legacy-mutation, adjacent-source
Area1. Both use one fresh path with four wavelengths and one mutation per pixel
per frame. Values below one favor the candidate. These are **160×90 signed
linear-RGB measurements**, not measurements of the native presentation images.

| Scene | Screen RGB MSE ratio (95% interval) | Screen low-frequency chroma ratio (95% interval) |
|---|---:|---:|
| Cornell | 0.1459 [0.1329, 0.1633] | 0.0409 [0.0335, 0.0513] |
| Prism | 1.1366 [1.0286, 1.2824] | 0.6063 [0.5107, 0.7337] |

Prism fails the RGB upper-bound gate of 1.05. Less low-frequency color noise
does not establish less total error. All 315 screening regional/channel mean
safeguards pass. The held-out study's 60 raw online-mean safeguards also pass;
these permissive finite-sample checks do not prove equality of converged images
or erase the earlier Prism warning.

A separate post-screen **descriptive** reduction uses the same held-out source
streams and four scalar references. It cannot select a candidate or overturn
the failed primary gate. Its unadjusted intervals are not simultaneous tests.

| Output | Cornell RGB MSE ratio (95% interval) | Prism RGB MSE ratio (95% interval) |
|---|---:|---:|
| Raw | 0.1287 [0.1191, 0.1416] | 1.0561 [0.9434, 1.1862] |
| Temporal only | 0.1288 [0.1117, 0.1626] | 1.3330 [1.1212, 1.5749] |
| Signed temporal + spatial | 0.1268 [0.1100, 0.1608] | 1.3332 [1.1213, 1.5752] |
| MetalFX | 0.5916 [0.5477, 0.6446] | 0.9648 [0.9247, 1.0105] |

Cornell improves in every output. Prism signed reconstruction has higher RGB
error even though its low-frequency chroma ratio is 0.6423. MetalFX's Prism RGB
interval includes one; these results do not establish preserved saturation or
fine reflection detail. Neither denoiser changes in this PR. The composite
candidate result also does not establish an incremental benefit from its MCMC
refresh alone.

The 16-pixel stencil covers different scene footprints at 160×90 and 1920×1080.
The native sheets are qualitative evidence, not a transfer of the percentages
above to 1080p. Smooth dispersive transmission can retain only one wavelength;
observer-only importance sampling cannot account for every wavelength-dependent
light path. Path-aware spectral importance and the interaction of reuse with
reconstruction remain follow-up work.

The complete native signed-temporal pipeline passes the predeclared 5% cost
gate in both scenes. For each run, the analysis uses linear-interpolated median
and p95 quantiles of 96 measured frame times after eight warm-up frames; the
reported ratio is the median of three paired candidate/baseline ratios. This
differs from the benchmark JSON's nearest-rank summary. All 1,248 frames across
the 12 timing runs completed, including 1,152 measured frames.

| Scene | Median time ratio | p95 time ratio | Candidate median frame time, three runs |
|---|---:|---:|---:|
| Cornell | 0.9834 | 0.9853 | 3.304–3.308 s |
| Prism | 1.0084 | 1.0059 | 3.318–3.325 s |

Throughput remains about 0.30 rendered FPS. Passing this relative cost gate does
not repair the failed quality gate or establish time to equal image quality.

Implementation checks include 11 focused Release tests with real M4 Max access,
three CLI contract checks, standalone hero and mutation/stencil sanitizer
checks, and independent estimator and artifact reviews. The PR records the
mandatory full Debug publication run and CI separately.

The retained local evidence is in `.cache/validation/spectral-color-repair-003`
(binding `6df5a18ab29208799c8d7333fba761a6e212da14ae4fb48083a1e76a566c037f`),
`.cache/validation/spectral-color-timing-001`, and
`.cache/validation/spectral-color-reconstruction-descriptive-001`.
Earlier preparation attempts remain intact. The first timing launch in 003
was rejected before rendering because the CLI rejects an explicit prefilter
option with conservative variance, even when its value is off. The separate
timing harness removes that redundant option, checks the effective false value,
and preserves the original jobs, ordering and gates.

The native presentation contains 12 full-resolution PNGs and two six-panel
sheets in `spectral-color-repair-003/presentation/`, with an `index.html` gallery
and a hash-bound manifest. Each sheet labels its 640×360 thumbnails separately
from the 1920×1080 rendered inputs. All six 128-frame source sequences completed.
Visual inspection still shows fine spectral noise in signed Prism output and
smoother spectral bands in MetalFX; the captures do not establish resolution
of every color or detail issue.
