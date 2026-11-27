# Golden rendering pipeline

The interactive target is native 1920×1080, one new camera path per pixel per
frame, and 20–30 genuine rendered frames per second on the 40-core M4 Max.
All eight stages have implemented baselines and bounded quality/timing studies.
The measured scene suite does not meet this target; broader stress coverage
remains open.
All shadow, reuse, mutation, guide and reconstruction work counts toward frame
time. Upscaled and interpolated display rates are reported separately.

The pipeline is native Metal traversal, wavefront spectral PT, Area ReSTIR
temporal reuse, optional weight-corrected MCMC, spatial reuse, and separate
denoising/presentation. The current four-scene study targets zero mutations
with budgets of 50 ms on Cornell/Prism and 100 ms on Pavilion. The viewer
exposes the sampled spatial policies for direct inspection.
[Zero-mutation performance and quality](zero-mutation-performance.md)

The original one-mutation configuration uses full replay and is selected in the viewer
with `--estimator area-replay --mutations 1 --mode temporal`. The viewer defaults
to independent sampling and signed temporal reconstruction because the current
full-replay reuse implementation increases both cost and denoised error in the
audited static fixtures. The candidate remains the optimization objective; its
implementation does not establish its performance or quality benefit.

The complete current surface and homogeneous-media
profile remains the transport target. The live renderer explicitly overrides
BDPT with PT; the offline renderer retains BDPT, authored film clamping,
compensated intersections and checkpoint behavior. Native triangle results have
a separately documented interactive numerical profile.

## Review sequence

1. Persistent GPU frame sessions and a reproducible headless benchmark.
2. Authoritative native triangle traversal with alpha, procedural geometry and media.
3. GPU wavefront queues for the current surface and medium models.
4. Area ReSTIR film/lens and full indirect-path reuse, with complete replay oracles.
5. Weight-corrected MCMC mutations with full-replay controls and ablations.
6. MetalFX denoising/upscaling and qualification of a reconstruction fallback.
7. A minimal native viewer with camera controls, captures and raw/denoised comparison.
8. End-to-end M4 Max tuning, sustained scene-suite measurements and qualification.

Each boundary is a focused PR, stacked where necessary and manually reviewed.
The first frame baseline is described in [frame rendering](frame-rendering.md).
The stage baselines now exist; their measured outcomes and remaining acceptance
limits are recorded in [pipeline qualification](pipeline-qualification.md).
Four audited sustained runs meet their duration requirement but also miss 20 FPS.

## Contracts

Raw spectral estimates remain independent of reconstruction and display edits.
Queue order cannot change random sample identity; incomplete, invalid or exhausted
work fails visibly. Reservoirs retain film/lens/wavelength state and use explicit
support-aware weights. Reconnection proposals require forward/reverse densities,
valid inverses and Jacobians; execution errors are not ordinary rejected proposals.
Fresh canonical proposals preserve coverage. Opt-in Area reuse defaults to one
mutation, with zero/two/four-step ablations; independent sampling performs none.

Immutable scene resources remain resident. Two completion-retained frame slots,
ordered histories and checked resource budgets govern lifetime. Camera motion
invalidates static accumulation while allowing validated temporal reprojection;
cuts and incompatible scene/lens/resolution changes reset affected histories.
The initial scope has static scenes and camera interaction, not animated geometry.

MetalFX's single surface guides approximate defocus, transmission and volume
correspondence. These cases require explicit reconstruction-quality tests rather
than changes to authored transport. Sorting is initially off and is promoted
only by complete-frame measurements. Interpolation remains optional and is not
counted toward the target rendering rate.

## Evidence

Qualification covers diffuse interiors, many lights/materials, mesh instances,
glossy and glass surfaces, defocus and homogeneous media. Cached Pavilion day and
night remain future stress coverage: no new Pavilion stress-performance runs are
included in this snapshot. Its historical quality failure and the deferred
investigation in [issue #52](https://github.com/rlefko/rypath/issues/52) are separate
evidence. Native 1080p p95 frame time
at or below 50 ms is the 20 FPS target; 33.3 ms is the 30 FPS target. Record misses,
startup, memory, actual work, thermal behavior and quality alongside successes.

The audited historical `0ef51ba` study includes 17 quality jobs (26,240 frames,
1,980 EXRs) and 108 native timing runs (3,024 frames). Independent/temporal has
lower post-cap physical MSE than Area-one/temporal in all six static fixtures.
Across three seeds, native complete-pipeline throughput spans 2.93–21.97 FPS for
independent and 0.48–6.66 FPS for Area-one. Only 2/90 primary runs meet p95 ≤50 ms;
none meet approximately 33.33 ms, and no scene/arm passes all three seeds.
See the [full tables and scopes](pipeline-qualification.md), including the
integration-specific MetalFX limitations and finite-reference uncertainty.

Full replay supplies a correctness control for this Area-domain adaptation; it
does not establish the efficiency of the papers' optimized reconnection methods.
Stronger importance sampling, replay/reconnection improvements and guide/filter
cost reductions need new paired quality and timing evidence.

Primary foundations: [wavefront PT](https://research.nvidia.com/sites/default/files/pubs/2013-07_Megakernels-Considered-Harmful/laine2013hpg_paper.pdf),
[GRIS](https://graphics.cs.utah.edu/research/projects/gris/sig22_GRIS.pdf),
[Area ReSTIR](https://graphics.cs.utah.edu/research/projects/area-restir/),
[MCMC mutations](https://arxiv.org/html/2211.00166),
[ReSTIR PT Enhanced](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/),
[ray reordering](https://meistdan.github.io/publications/raysorting/paper.pdf), and
[Apple MetalFX integration](https://developer.apple.com/videos/play/wwdc2025/211/).
Their published results do not establish rypath performance or correctness.
