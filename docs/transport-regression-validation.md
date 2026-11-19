# Pavilion importance transport regression

This record follows the adverse raw night-floor results in the
[sampling/convergence study](sampling-convergence-validation.md). The original
measurements remain unchanged. The correction below targets the existing
camera-side radiance model; final scene-level quality remains a separate check.

## Night contribution

On frozen candidate v5, night seed 24 at 320×170 and 512 spp, pixel (97,101)
accounted for 91.96% of floor variance and 97.32% of its heavy-tail statistic.
An exact checkpoint replay and all 512 owner-local packets isolated absolute
sample 119. Its sole nonzero camera strategy was the connection between three
light vertices and three camera vertices, `(s=3,t=3)`.

The connected endpoints lie on the same planar triangle. Their geometric
normals are exactly perpendicular to the connecting direction, but their bump
normals give nonzero shading cosines. The short segment has squared length
0.00166202 and an uncorrected geometry factor of 190.35837. Its MIS weight is
0.8617264. This is a transport inconsistency, not a reconstruction-filter or
display artifact. Same-AS frozen/traced/frozen observations retained identical
native packet outputs, splats and every used vertex byte before interpretation.

The radiance model uses the shading-normal cosine. Its importance adjoint needs

```
C = abs(dot(wo, ns) * dot(wi, ng)) /
    abs(dot(wo, ng) * dot(wi, ns))
```

Here `wo` points toward the preceding light-path vertex and `wi` toward the
next vertex. At the offending connection `dot(wi,ng)` is exactly zero and the
denominator is nonzero, so the corrected contribution is zero without a clamp.
The correction applies to accepted importance continuations, including delta
events, and to connection/splat endpoints. It preserves the sampled joint
coating-walk weight, directional PDFs, MIS, sample indices and film accounting.
Radiance-mode BSDF evaluation remains unchanged.

The implementation retains the exact local cosines used by BSDF sampling.
It combines endpoint `f*C*abs(dot(wi,ns))` before materializing the removable
grazing singularity, and uses mantissa/exponent arithmetic to avoid intermediate
overflow and underflow when the final weight is representable. Exact singular
directions use the explicit zero convention; nonfinite inputs and unrepresentable
final normal-range weights fail visibly. Metal may flush final subnormal values
to zero. There is no near-grazing cutoff or new raw-film clamp. Incoming geometric
grazing directions can still have large weights; this correction does not make
arbitrary bump-normal models energy conserving or establish convergence parity.

This follows the adjoint derivation in
[Veach, section 5.3.2](https://graphics.stanford.edu/papers/veach_thesis/thesis.pdf)
and the [PBRT third-edition path-space discussion](https://www.pbr-book.org/3ed-2018/Light_Transport_III_Bidirectional_Methods/The_Path-Space_Measurement_Equation).
Pinned PBRT v4 `b0bdf461` omits this factor in BDPT. The correction deliberately
differs from that implementation to make light tracing agree with rypath's
existing camera-side radiance model; it is not a claim of PBRT v4 BDPT identity.

## Development checks

On Apple M4 Max, the transport-only shader passes 249 native helper cases and
64 fixed scene renders: eight seeds, flat/tilted normals, direct/indirect fixtures,
and PT/BDPT pairs. The mean rule was fixed before execution at six paired standard
errors plus 2% of the PT mean. All four comparisons pass. An independent diffuse
quadrature predicts a tilted/flat radiance ratio of 0.505696, also passing. A
depth-one zero control confirms the tilted corner's signal requires indirect
transport. Raw checkpoint planes are retained independently of presentation.

The first native helper execution failed on a final result of `2^-140` flushed
to zero. Its capture remains preserved. The revised native check retains that
case with an explicit subnormal-output policy and adds a normal-result stress
case; the portable oracle still checks subnormal results. No scene mean threshold
or seed was changed.

An endpoint-disabled control fails the original tilted-direct mean gate
(BDPT/PT 1.031929). The original continuation-disabled corner and a separate
256-spp spotlight fixture both pass their gates, demonstrating insufficient test
power. Those outcomes remain retained. A final fixed 8192-spp spotlight experiment
uses the same eight seeds, geometry and six-SE-plus-2% rule: corrected BDPT/PT is
0.998213 and passes; disabling the continuation correction gives 0.956184 and
fails. This is a development test of branch sensitivity, informed by the earlier
runs, not an independent Pavilion convergence result.

The extended check takes minutes (215.7 seconds for the corrected transport-only
shader in the observed Debug-host run), so it is separate from the default CTest
suite. Run it for changes to importance continuation:

```sh
MTL_DEBUG_LAYER=1 ./build/release/rypath_packet_render_tests --shading-normal-spot-extended
```

The 256-spp `--shading-normal-spot` diagnostic does not substitute for this check.
These branch-disabled controls use renderer 15. Pavilion scene-level
measurements remain separately identified.
