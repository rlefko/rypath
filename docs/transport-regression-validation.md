# Pavilion transport and triangle regressions

This record follows the adverse results in the
[sampling/convergence study](sampling-convergence-validation.md). The original
measurements remain unchanged: correcting repeated Halton light paths improved
the authored-clamped night image but increased raw floor variance. A separate
four-pixel day difference prevented an exact repeatability claim.

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

## Day intersection

Same-AS repetitions were stable, but fresh native acceleration structures
selected different near-edge primitives for identical input rays. Independent
exact-rational tests of the supplied binary32 geometry established two cases:

| Ray | Native alternatives | Independent geometric result |
| --- | --- | --- |
| Pixel (1251,68), sample 141, second walk hit | Primitives 773 and 777, equal returned float distance | 773 is nearer by 3.57738e−7 |
| Pixel (1556,455), sample 74, first walk hit | Primitives 556 and 562 | Both miss; nearest is 601 |

These are perpendicular faces meeting at duplicated-position edges, not duplicate
coplanar triangles. Sorting native hit IDs at equal float distances would not fix
the second case. A direct call to pinned PBRT's triangle routine also accepts the
second ray's false edge candidates, so PBRT is not an exact geometric oracle here.

A bounded native-query experiment retained all independently valid hits across
38 fixed original/ULP-neighbor rays and four fresh structures. Candidate order
changed while candidate sets remained stable. The production approach retains
native acceleration structures, revalidates offered triangle candidates with
compensated arithmetic, and selects the nearest compensated parameter. Computed
ties use stable instance/geometry/primitive IDs. Forward affine products remain
compensated before projection. Alpha receives the validated coordinates; shadow
queries terminate only after a validated accepted blocker. Native rounded hit
distances do not prune subsequent candidates.

The arithmetic adapts ray-aligned projected edges described by
[Woop, Benthin and Wald](https://jcgt.org/published/0002/01/05/) and PBRT, using
rypath's existing float-pair representation. This is a finite numerical policy,
not a proof of exact predicates or universal watertightness. Unsupported arithmetic
returns a visible invalid-hit result. Candidate revalidation cannot recover a
true intersection omitted by native traversal; the candidate-coverage evidence
is bounded by the tested rays and transforms.

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
These branch-disabled controls use renderer 15; final combined-candidate checks
and Pavilion measurements remain separately identified.

The first combined candidate, renderer 16, passes the full-frame day seed-0
pilot and exact repeatability of all 8,192 packets from eight day pixels across
four fresh renderer instances. Its night seed-0 pilot stops after 96/512 samples
at pixel (252,69), absolute sample 97, with an invalid-hit diagnostic. That failed
attempt is retained without a successful image or statistical record. The full
48-endpoint re-evaluation has not started on this failed candidate.

An instrumented replay identifies an overstrict arithmetic guard. Its original
26,864-byte workspace is identical to the uninstrumented shader for both the
passing sample 96 and failing sample 97. The captured operation multiplies a
low component by a high component while validating native instance 52,
primitive 36. The rounded product is a normal `-5.921830585e-30`; even its exact
residual is normal, with leading exponent -123. The old guard instead rejects
the exact product's lowest nonzero bit at exponent -139. That bit position does
not determine whether a floating-point residual is subnormal. The captured
triangle and ray also have an exact-rational intersection at `t=0`, inside all
three edges. This establishes a validator regression rather than an unsupported
input or evidence for silently discarding that candidate.

Renderer 17 corrects this range policy: the compensated high-product operation
checks the actual residual, and deliberately rounded cross products check their
returned value. Genuine unsupported arithmetic still fails visibly. The failed
renderer-16 pilot remains preserved; final repaired-candidate validation is
recorded separately.

The repaired native arithmetic passes 5,049 scalar/pair cases, including the
captured operation and genuine unsupported residuals. The first direct triangle
test incorrectly required an exact hit at the mathematical `t=0` endpoint.
An unchanged-kernel dump instead returns a valid miss. Two independent
exact-rounding reconstructions find all three edge signs positive and no invalid
arithmetic, but finite-pair cancellation gives `t=-4.38515e-20`, outside the
unchanged lower bound of zero. This is within the declared finite numerical
policy, which does not guarantee exact endpoint inclusion. The failed test and
dump are retained; the test characterizes this boundary and adds strict nearby
inside/outside cases without changing production arithmetic or expanding bounds.

Frozen renderer-17 candidate v7 (`22cdfe7175f2` source aggregate) completes the
previously failing night sample 97 with valid guards; sample 96's public packet
bytes remain identical to the retained control. The full-frame night seed-0
pilot then completes 320×170 at 512 spp, with finite raw and authored-clamped
FLOAT EXRs. Its 537.93-second wall time is a validation observation, not a
controlled performance comparison. The four-instance day replay also matches
all 8,192 packets and all eight owner-film reductions exactly.
The day seed-0 pilot completes 320×170 at 256 spp in 122.12 seconds; both FLOAT
planes are byte-identical to the successful renderer-16 day pilot. Neither pilot
is a full-resolution output or an ensemble quality result.

This candidate passes 148 CPU checks in each of Debug, Release and ASan/UBSan,
225 Python checks, and all seven focused geometry suites with Metal API
validation. These checks remain distinct from the complete GPU suite, the
full-resolution images and the 48-endpoint statistical re-evaluation.
