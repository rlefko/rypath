# Hero wavelength PT

Vacuum PBRT path tracing now offers the four-wavelength estimator from
[Wilkie et al., Hero Wavelength Spectral Sampling (2014)](https://jo.dreggn.org/home/2014_herowavelength.pdf).
This changes the sampling estimator, not the scene's spectra or scattering model.
Raw spectral accumulation remains separate from film clamping, reconstruction
and display transforms.

## Sampling contract

A uniform hero wavelength lies in [360, 830) nm. Three companions are offset by
one quarter of that interval with periodic wrapping. All four retain their
original sensor density, 1/470 per nm, throughout the path. The hero selects the
direction; each supported material evaluates the physical numerator and proposal
density at every active wavelength.

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
uses automatic selection. Eligible vacuum PT/volpath scenes with depth at most
65 use hero transport. Actual BDPT retains the existing packet implementation;
active media and greater depths retain scalar transport where supported.
Declared nonzero media conservatively prevent automatic hero selection, even
when a particular medium is not used. Explicit unsupported selections fail with
a diagnostic. Authored depth and finite film clamping are not silently removed.
Live benchmark film/integrator overrides remain explicit in their provenance.

The PBRT translation library retains its historical default when no transport
selection is supplied. Application entry points supply the chosen policy during
translation, so depth admission and filter/lens provenance match actual execution.
The existing `packet` mode remains available as a regression control; its shared
direction proposals are distinct from the new hero-driven estimator.

Offline and wavefront hero rendering share the rolling PT state. The offline
output uses an 80-byte result per lane rather than allocating legacy camera and
light path arrays. Wavefront scheduling and Area replay use the same estimator.
Metal function constants remove the unused spectral estimator from production
pipelines, while standalone probes retain runtime mode selection.
All wavelength draws remain part of the replay tape. Renderer contract 28 and
sampling contract 10 invalidate incompatible checkpoints.

## Validation scope

Independent portable tests enumerate small joint wavelength/path distributions,
including wavelength-dependent direct-light proposals, black hero channels,
dispersive events, compensated collapse and earlier emission. Negative controls
reject local-only spectral weights and hero-only light MIS. Numerical tests cover
likelihood underflow and regrowth, subnormal rounding, and tiny MIS weights
multiplied by large contributions.

Native BSDF probes compare against independent Fresnel, Snell and GGX equations.
Native integration checks compare offline and wavefront results across seven
queue schedules, Area cold/warm replay, high absolute sample indices, positive
dispersive transmission and an actual 65-interface path. Matched-index and
depth-64 controls distinguish successful transport from an all-black result.

These tests establish bounded regression evidence. They do not prove unbiasedness,
qualify every material or establish the 1080p frame-rate target. Scene quality and
timing evidence must identify its source revision, sampling budget and raw output
space. Full-replay Area reuse remains the documented adaptation in
[the ReSTIR record](restir-importance.md); hero wavelength sampling does not add
the Enhanced paper's optimized path reconnection machinery.
