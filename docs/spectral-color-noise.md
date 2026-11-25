# Spectral color noise

This repair changes sampling and reuse covariance, not scene spectra, geometry,
materials, exposure, denoisers or path count. Each fresh path still carries four
wavelengths. Smooth dispersive transmission may retain only the hero, and coated
materials retain the documented compensated hero-only fallback.

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

The local study `spectral-color-repair-002` predeclares 336 raw screening jobs:
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
before observing outcomes. Scene-quality and timing results are pending.

Final Cornell and Prism sheets use native 1920x1080, seed 51001, 128 static
submissions and frame 127. PT, Area0 and Area1 raw panels accompany three
reconstructions of the identical Area1 stream: temporal-only, signed spatial
plus temporal, and native MetalFX. Presentation uses fixed exposure 1,
Reinhard and sRGB; signed FLOAT source images remain unchanged.
