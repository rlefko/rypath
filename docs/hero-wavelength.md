# Hero wavelength PT

Vacuum PBRT path tracing now offers the four-wavelength estimator from
[Wilkie et al., Hero Wavelength Spectral Sampling (2014)](https://jo.dreggn.org/home/2014_herowavelength.pdf).
This changes the sampling estimator, not the scene's spectra or scattering model.
Raw spectral accumulation remains separate from film clamping, reconstruction
and display transforms.

## Sampling contract

The default hero sampler uses PBRT's visible-sensitivity inverse CDF on
[360, 830] nm. Four wavelengths are separated by quarters in probability space;
integer shifts preserve every point of the renderer's 24-bit uniform lattice.
Each lane retains its own wavelength PDF through sensor conversion.
`--hero-wavelengths uniform` preserves the original equal-nanometer control.
The hero selects the direction; each supported material evaluates the physical
numerator and proposal density at every active wavelength. See the
[spectral noise repair](spectral-color-noise.md) for the derivation and qualification.

The implementation follows the paper's cumulative balance construction. For
prefix likelihoods P[k] and physical path numerator F[j], the running mixture
throughput is 4 F[j] / sum(P). Each event multiplies every prefix likelihood by
its own proposal density. A shared exponent representation preserves tiny
alternative likelihoods that can become important later. It does not extend the
range of stored binary32 physical throughput. Direct-light and BSDF sampling use
the corresponding joint balance weights; the weight is applied before rounding
the resulting contribution to binary32.

Aligned delta events retain companions. Dispersive delta transmission assigns
zero support to companions whose refracted directions differ. Already accumulated
emission is unaffected by that later event. Roulette uses a common, cyclically
invariant survival probability before both future direct-light sampling and
continuation, with per-wavelength eta compensation. Arrival emission precedes
that gate. Unrepresentable roulette heuristics disable the optional gate rather
than discard a path.

Coated diffuse and coated conductor currently expose a sampled hidden-walk ratio
without its full joint density. Those events explicitly collapse future transport
to the hero with inverse-posterior compensation. Their directional MIS proxy is
kept separate from the hidden-walk weight. This is a documented fallback, not a
claim of four-wavelength coating proposals.

## Execution and selection

`--spectral-transport auto|scalar|packet|hero` selects the mode in the renderer,
frame benchmark, reconstruction diagnostics and qualification tools. The viewer
also exposes these controls; automatic selection preserves the prior scalar/packet policy.
Hero is explicit for vacuum PT/volpath scenes with depth at most 65. BDPT retains
the existing packet implementation; active media and greater depths retain their
supported alternatives. Declared nonzero media conservatively prohibit explicit
hero selection, even when a particular medium is not used. Unsupported selections
fail with a diagnostic. Authored depth and finite film clamping are not silently removed.
Live benchmark film/integrator overrides remain explicit in their provenance.

The PBRT translation library retains its historical default when no transport
selection is supplied. Application entry points supply the chosen policy during
translation, so depth admission and filter/lens provenance match actual execution.
The existing `packet` mode remains available as a regression control; its shared
direction proposals are distinct from the new hero-driven estimator.
An API caller may explicitly set the scene's transport to hero; `auto` preserves
that choice while checking its medium and depth requirements.

Offline and wavefront hero rendering share the rolling PT state. The offline
output uses an 80-byte result per lane rather than allocating legacy camera and
light path arrays. Wavefront scheduling and Area replay use the same estimator.
Metal function constants remove the unused spectral estimator from production
pipelines, while standalone probes retain runtime mode selection.
All wavelength draws remain part of the replay tape. Renderer contract 29 and
sampling contract 11 invalidate incompatible checkpoints.

## Historical renderer 28 validation scope

Independent portable tests enumerate small joint wavelength/path distributions,
including wavelength-dependent direct-light proposals, black hero channels,
dispersive events, compensated collapse and earlier emission. Negative controls
reject local-only spectral weights and hero-only light MIS. Numerical tests cover
likelihood underflow and regrowth, subnormal rounding, and tiny MIS weights
multiplied by large contributions.

Native BSDF probes compare against independent Fresnel, Snell and GGX equations.
Native integration checks compare offline and wavefront results across seven
queue schedules, Area cold/warm replay, high absolute sample indices, positive
dispersive transmission and an actual 65-interface path. Successful parallel-slab
paths must carry the analytical radiance: 4 times the emitter value in the sole
supported dispersive lane, or the emitter value in every lane for constant eta.
Cyclic exit-angle probes include wavelengths on both sides of total internal
reflection. Matched-index and depth-64 controls distinguish successful transport
from an all-black result.

These tests establish bounded regression evidence. They do not prove unbiasedness,
qualify every material or establish the 1080p frame-rate target. Scene quality and
timing evidence must identify its source revision, sampling budget and raw output
space. Full-replay Area reuse remains the documented adaptation in
[the ReSTIR record](restir-importance.md); hero wavelength sampling does not add
the Enhanced paper's optimized path reconnection machinery.

## Measured scope and adoption decision

The following measurements used source `758a12d` on M4 Max, native 160x90
wavefront rendering, fixed texture footprint 1 and material seed 0, independent
paths, and signed unexposed linear RGB. They exclude Area reuse and reconstruction.
The timing screen enabled Metal API validation, so these are instrumented
comparisons, not production FPS or native-1080p acceptance.

The completed screen used 16 paired seeds (63001-63016), eight warmup and 32
measured frames per run. Its quality observation is the final single estimate,
sample 39, rather than a 40-sample accumulated image. Variance is pixelwise sample
variance across seeds, averaged over RGB and pixels. Intervals resample whole
seeds 2,000 times; they are descriptive 95% bootstrap intervals.

| Scene | Hero/scalar raw variance | Interval | Median GPU time, scalar/hero | Variance x GPU-cost ratio |
| --- | ---: | --- | --- | ---: |
| Cornell | 0.0892 | 0.0810-0.0995 | 2.581/4.408 ms | 0.152 |
| Prism F11 | 0.974 | 0.840-1.125 | 1.975/4.314 ms | 2.128 |

Cornell has a substantial spectral variance reduction in this screen. Prism has
no established variance reduction and costs more. These are variance comparisons,
not reference-image MSE, bias tests, equal-time convergence or universal speedups.

A separate held-out Prism diagnostic used 16 paired seeds (73001-73016), each
with 128 raw frames (0-127). Each seed's complete raw mean is one observation.
Whole-image and four-quadrant RGB comparisons used the fixed safeguard
`abs(delta) <= max(5*paired_SE, 0.02*pooled_magnitude, 1e-5)`. **14/15 passed.**
Bottom-left green failed: scalar mean 0.004878756, hero mean 0.005091145,
delta 0.000212389, paired SE 0.0000410001, limit 0.0002050003. All whole-image
channels passed. About 91.7% of that regional difference came from primary-glass
pixels. Independent receipt, finite-payload, per-frame mean, mask and reduction
audits found no artifact or configuration mismatch.

A controlled diagnostic at local source `6a521c8` removed both roulette
termination and compensation in scalar surface transport and the hero gate.
It preserved all other source, random addresses, seeds and frame counts, adding
exactly 32 runs. This diagnostic change is not part of production. Five declared
contrast families and protected/unprotected contributions retain all outcomes.
With roulette disabled, all 15 hero-minus-scalar pointwise 95% t15 intervals
include zero. The affected green delta becomes 0.000117838 with SE 0.0000638837;
the change in the difference is -0.000094551 with SE 0.0000452989. The observed
reduction is 44.5%, but its own interval includes zero. Some within-estimator
intervention contrasts also exclude zero; these remain descriptive observations,
not additional acceptance tests or evidence to discard.

The intervention supports a partial roulette-variance explanation. It does not
prove sole causation, unbiasedness or equivalence, and the original failed
safeguard remains failed. Source review found no concrete estimator defect, and
the stronger analytical glass/TIR tests passed. Hero therefore ships as an
explicit option while automatic selection retains its prior behavior. Default
promotion requires further qualification; repeating seeds until a safeguard
passes is not the adoption policy.

Retained local records are `hero-quality-001`, `hero-prism-mean-001`,
`hero-prism-mean-audit-001`, and `hero-prism-roulette-001` under
`.cache/validation/`. Each binds its source, binaries, scene, protocol and captures.
