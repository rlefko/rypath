# ReSTIR importance sampling repair

Area contract 5 retains contract 4's radiance-like target and source-target MIS,
which replaced the contract-3 bounded target and equal-weight reuse. The former target varied by
at most ten percent, so it intentionally surrendered most of the importance
selection that motivates ReSTIR. Its historical correctness and timing results
in [area-reuse.md](area-reuse.md) do not establish a useful MSE reduction.

This remains our full-replay adaptation of [GRIS](https://graphics.cs.utah.edu/research/projects/gris/sig22_GRIS.pdf),
not the complete [Area ReSTIR](https://graphics.cs.utah.edu/research/projects/area-restir/)
algorithm. Fractional footprint matching, lens reconnection and local path
reconnection remain absent. Physical path evaluation, spectral transport,
independent sampling, denoising and tone mapping retain their separate contracts.

## Target and stored weight

For an unexposed signed XYZ leaf contribution `F`, the positive target is

```text
t(F) = max(FLT_MIN, 1e-6 * target_scale)
       + ((abs(X)/4 + abs(Y)/4) + abs(Z)/4)
```

Quarter-L1 is our proxy, not a target prescribed by the papers. It admits signed
spectral contributions and avoids overflowing the sum of finite XYZ components.
Mantissas and exponents are evaluated separately; neither radiance nor a
contribution weight is clipped. Each selected sample stores its target `t` and
normalizer `Z`; its conceptual contribution weight is `Z/t`. The implementation
forms `Z*F/t` in scaled arithmetic rather than materializing a possibly
overflowing intermediate `Z/t`.

## Source-target MIS

All functions below are over the same finite replay domain. Every shifted leaf
is fully reevaluated at its destination. For fresh sample `u_f`, previous selected
sample `u_h`, current target `t_c`, previous-context target `t_h`, and previous
normalizer `Z_h`, warm temporal scores are

```text
s_f = t_c(u_f)^2 / (t_c(u_f) + 20*t_h(u_f))
s_h = 20*Z_h*t_c(u_h) / (t_c(u_h) + 20*t_h(u_h))
```

The coefficients 1 and 20 are fixed, exogenous policy choices, not effective
sample counts or a claim that twenty independent paths were evaluated. A cold
frame uses its fresh sample alone. Using an age or confidence inferred from a
selected sample as a coefficient would need a separate correctness argument.

The extra reverse replay evaluates the fresh payload in the complete previous
successful context. Parameters, texture camera and camera-relative mapping
buffers are owned with the history bank; reusable frame-slot buffers cannot
stand in for that context. Publication swaps context and reservoir together.
A failed same-epoch attempt retains prior history. An epoch change invalidates
eligibility before any potentially failing context allocation.

For four fixed neighbor slots `j`, spatial scores are

```text
s_j = Z_j*t_c(u_j) / (4*(t_c(u_j) + t_j(u_j)))
s_c = sum_j Z_c*t_c(u_c) / (4*(t_c(u_c) + t_j(u_c)))
Z_out = s_c + sum_j s_j
```

The source-MIS factors form a pointwise partition of unity, assuming properly
weighted inputs and fixed, exogenous source functions and neighbor stencil. Correlated inputs
and clamped duplicate slots retain their scheduled shares. There is **no extra
division by pool size**. Pairwise denominators avoid the old constant-coefficient
`destination_target/source_target` amplification when a source is nearly dark
and its destination bright. This is not a global bound on `Z` or output variance.

Four reciprocal target planes reuse the existing neighbor evaluations: the
opposite-direction cache at a neighbor provides `t_j(u_c)`. All four immutable
temporal-cache passes complete before the center contribution is finalized.
Clamped self-neighbors use the center target directly. The cache costs 16 bytes
per pixel per frame slot, or 31.64 MiB at 1920×1080.

## Mutations and final output

The existing symmetric film/lens/wavelength/tape proposals still run between
temporal and spatial reuse. Normalizer MH correction uses the **actual rounded
dyadic acceptance probability**, not the ideal real-valued ratio. Accepted
normalizers are checked before consuming entropy; equality and rejection
preserve the original normalizer bits. See [the MCMC paper](https://arxiv.org/html/2211.00166)
for weighted mutation theory and [the existing proposal record](mcmc-mutations.md)
for our finite-coordinate adaptation. Pinhole lens moves remain latent no-ops;
tail moves regenerate a whole path. Stronger targets do not make these proposals
equivalent to the paper's reconnection-vertex mutations.

The default Area raw output is `conditional-mean`: sum the weighted contributions
already evaluated during final spatial reuse, `sum_i s_i*F_c(u_i)/t_c(u_i)`.
The `selected` ablation outputs `Z_out*F_c(u_selected)/t_c(u_selected)` instead.
Both retain exactly the same selected payload, normalizer and history. This
Rao–Blackwellization removes the final categorical choice's conditional variance;
it is neither a denoiser nor five independent SPP. It is especially relevant to
spectral color noise that persists when only one path's spectral direction is
selected.

Proper weighting and the Rao–Blackwell variance reduction hold in real
arithmetic. Scores, normalizer corrections, contributions and signed sums incur
floating-point rounding. Exact floating-point or success-conditioned unbiasedness
is not claimed. Contract 5 supports subnormal signed contributions and sums;
integer significand rounding implements gradual underflow for these outputs,
including ties to even and values that round to signed zero. Signed addition
scales finite operands into the normal range before arithmetic. This avoids
Metal's permitted flush-to-zero behavior for subnormal operands/results
([MSL specification, §8.1 and §8.5](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)).
Products and quotients still round their intermediate mantissas; final conversion
rounds that represented value rather than the infinitely precise formula.
Positive scores, targets and normalizers retain the normal-only domain.
Overflow and other values outside these domains invalidate the attempt.
The renderer does not drop a candidate, clamp it, or silently retry.

## Accounting and validation

The schedule evaluates `(5+n)*pixels` complete paths after a reset and
`(7+n)*pixels` with history, for `n` mutations. One fresh path per pixel remains
distinct from replay work. The benchmark records contract 5, `quarter-l1-v1`,
`source-target-mis-v1`, temporal coefficient 20 and the output mode. Independent
output records these as inapplicable. The benchmark accepts `--area-output selected`
or `--area-output conditional-mean`. Viewer, qualification and reconstruction
diagnostics integration belongs to their subsequent implementation stages.

Portable checks cover scaled arithmetic, independent scalar formulas, random
normalizers over multiple reuse/mutation generations, exact rational means and
second moments, and opposite-importance rare tails. A standalone compute-only
Metal probe checks actual GPU dyadic thresholds, extreme support and failures
before entropy consumption. Integration oracles cover analytic cold spatial
stencils, clamped duplicates, unchanged history under output marginalization,
changing-camera temporal denominators, slot reuse and failed publications.

Rendered MSE and equal-work/time qualification are pending. Synthetic variance
examples and passing mean tests are not evidence of a scene-suite speedup.
MCMC's correlation reduction must also be distinguished from the combined
estimator's raw MSE reduction; adding a mutation alone does not guarantee a
large MSE improvement.

This backport targets the MCMC stage. Earlier validation in the Area/MCMC records
measured the superseded bounded-target estimator. Native validation and independent
review of this adapted host integration are pending; no current-revision GPU or
scene-quality result is claimed here.

Upstream evidence at `fe93cca` identified a cold Pavilion-day failure: weighted
XYZ components near `(6.4771e-39, 7.0811e-39, 4.1524e-39)` were representable
subnormals rejected by contract 4. That upstream native frame subsequently
completed with API validation, and nine focused Release tests passed. This is
inherited evidence for the numerical repair, not checks on this backport or a
scene-quality/performance qualification. New probe cases retain those operands,
rounding boundaries, signs, addition edge pairs, random pairs and exact cancellations.
