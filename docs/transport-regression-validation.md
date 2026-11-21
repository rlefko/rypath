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

The subsequent full Release GPU suite passes 58/68 checks. Seven failures involve
invalid native hits in lighting fixtures with very narrow camera footprints;
the Sobol and grazing-conductor fixtures return successful but unexpected paths,
and the medium boundary stress times out. Long scene measurements are held while
these regressions are investigated.

A fixed point-light replay confirms another overstrict range check. Frozen and
instrumented shaders retain identical original sample/guard bytes and uploaded
parameters; an unmodified lens probe independently matches the captured ray.
Inside the triangle's barycentric division, a rounded cross product multiplies
`-1.5407439555e-33` by `4.5810366102e-27`. Its exact value is approximately
`-7.0582044671e-60`, which correctly rounds to negative zero even without
subnormal flushing. The guard incorrectly rejects that zero. Correctly rounded
zero requires `abs(a*b) <= 2^-150` under binary32 round-to-nearest, ties-to-even;
this is distinct from flushing a representable nonzero subnormal. The independent
boundary/random oracle checks 112,960 pairs. This attribution directly covers
the captured point fixture; the remaining failures require their own evidence.

Host-only phase logging completes all eleven ordinary medium fixture groups
before the final 4,097-plane boundary-exhaustion case stalls. Exhausting an
uncommitted candidate query at each of 4,096 null crossings predicts quadratic
work in this fixture. The phase trace does not separately measure AS preparation
and GPU traversal. The historical 1.95-second full-suite result and current
180-second timeout remain separate performance evidence.

The default medium regression now checks an explicit request limit of eight:
eight native crossings must succeed, and a ninth must produce the specific
boundary-exhaustion diagnostic with no returned samples. That real native
failure passes through the existing progressive transaction driver, which must
leave the entire fresh Film unchanged. The production limit remains 4,096;
`rypath_medium_render_tests --boundary-stress` retains the original full-limit
case as an isolated command. After the traversal repair below, the normal suite
also retains this original full-limit case.

The bounded medium test passes with Metal API validation in 9.26 seconds. A
separate fixed grazing-continuation witness and the conductor rendering/BSDF
suites pass 2/2 in 21.00 seconds. These development checks precede the combined
renderer-18 geometry changes and do not establish that revision's full acceptance.

## Renderer-18 regression repairs

A second admitted replay isolates the area-light failure. All 16 original samples,
parameters and guards match; the independent lens probe matches the failing primary
ray. A retained high-product range check rejects the exact residual `-2^-129`,
half a primary ULP, at an exact interior intersection `t=4194304/275899`.
This is a meaningful nonzero subnormal, unlike the correctly rounded zero above.
The capture records the classifier failure, not an observed hardware FMA result.

The Sobol replay retains the complete original sample prefix, parameters and
lens controls. Its third query offers only the wrong top-plane triangle. Exact
rational evaluation puts that candidate outside by barycentric `-7.6118863e-11`,
while the unoffered adjacent triangle is inside by `+7.6118863e-11` at
`t=33439726/11863283`. Revalidation correctly rejects the offered candidate; it
cannot recover the omitted neighbor. This attribution is to the admitted query,
not a claim about every unobserved internal event in the original shader.

The conductor replay instead reaches the intended primary triangle and returns a
valid local BSDF null: `wi.z=-5.8496212e-9`. More precise barycentrics changed its
almost-tangent shading frame, invalidating the old test's positive-local premise.
The original scene/sample remains as accounting coverage. A fixed extracted
frame/direction now tests the renderer's shared continuation helper directly:
positive local cosine must survive a nonpositive rounded world dot without
changing weight, PDF or acceptance. The test explicitly verifies that sign
inversion, so a world-dot support check would fail it; no new seed was selected.

Renderer 18 uses Metal bounding-box primitives for triangle candidate generation,
retaining native acceleration, instancing, original corners and primitive IDs.
Proven zero-area slots are explicitly empty. Logical triangle tags still govern
alpha and shading. Canonical sign-invariant pair multiplication removes an
independent operand-order asymmetry; an alternate ordering preserves the union
of the two formerly supported local arithmetic domains.

Exceptional numeric range recomputes the complete triangle from its original
inputs using pairs of integer binary24 values with independent exponents bounded
to ±100,000. Scalar operations round to nearest, ties to even; high-product errors
retain their exact 48-bit product residual. Full TwoSum renormalization retains
gapped components without requiring a magnitude ordering. Raw invalid/subnormal
inputs and unrepresentable public hit distances still fail visibly. A single
53-bit fallback was rejected because it lost the captured axial triangle's tiny
nonzero barycentric correction. The adopted pair remains finite precision; it
is not an exact-predicate or universal watertightness claim.

Closest-distance comparison sums the four signed dyadic components exactly,
including widely separated low terms. Pruning uses a strict endpoint beyond a
conservative forward-transformed world-box exit, rather than a native or rounded
triangle hit distance. Query resets retain the selected hit and original primitive
bounds; unsupported intervals disable pruning. This still relies on bounded native
box coverage and finite-predicate acceptance, especially under ill-conditioned
transforms.

Development renderer-18 Release checks pass all ten previously failing suites
with Metal API validation. The direct arithmetic probe passes 5,049 original
retained-product cases, 916 rounded-zero cases, 12 multiplication symmetry cases,
136 independently generated exact-rational rounding vectors and the captured
triangle/range fixtures. Native box tests pass 26 rays, including six Area/axial
cases under exact reflection, nonuniform scaling and axis permutation. The prior
Pavilion/transformed-candidate suite also passes. The isolated original
4,097-plane exhaustion case completes in 4.59 seconds with the required native
failure and unchanged Film. This is a completion observation, not a matched
performance comparison against the historical timeout. The full case is restored
to normal regression coverage; broad final-revision validation and scene costs
remain separate requirements.

The first combined renderer-18 Release API-validation run at `fbe7a57` passes
63/64 native tests in 292.96 seconds, including all ten original failures. Its
remaining failure is a test assumption: restarting traversal can offer an accepted
triangle to alpha more than once. Production alpha is deterministic for those
unchanged inputs. The probe now records the primitive IDs seen by alpha, allowing
repeat visits while explicitly rejecting false candidates 556/562 and any other
unexpected ID. Rejected-hit, invalid-alpha and shadow-termination controls retain
their distinct expected IDs and call counts; final hit identity and coordinates
remain checked. This changes test instrumentation, not transport behavior. The
original failed run is retained separately from validation of the revised probe.
The revised masked and analytic probes pass 2/2 with API validation in 13.44
seconds. Both affected hosts build in all three presets and pass focused
Clang-Tidy and Apple analyzer checks.

An immutable `fbe7a57` renderer-18 snapshot also passes the fixed 8,192-spp
shading-normal spotlight check with API validation in 501.00 seconds. Its 32
PT/BDPT renders span the unchanged eight seeds and flat/tilted fixtures; both
paired-mean gates and the zero-direct control pass. The tilted paired difference
is `-0.000134977` against tolerance `0.0020597`. This snapshot precedes only the
alpha test-instrumentation changes above; its source and binaries remain
unchanged during execution. These bounded checks do not establish Pavilion
convergence parity.

The completed candidate at `c3489bf` passes 220 checks per preset: 151 CPU,
64 native and five GPU/CLI checks in Debug, Release and ASan/UBSan. Debug and
Release run all native checks with Metal API validation; CLI checks run without
its diagnostic banner. Native suite times are 278.10, 239.41 and 308.24 seconds
respectively; these are validation timings, not renderer performance comparisons.
All source and executable hashes remain unchanged within each run. The host is
an M4 Max with 128 GiB memory, macOS 26.6 (`25G5065a`), Apple Clang 21.0.0,
Metal compiler 32023.883, CMake 4.0.3 and Ninja 1.13.2.

The frozen renderer-18 source manifest is
`2e9a2a5240790496a8e9bf0f2711458f47493289cffa7a7d67efb7571773dcc3`;
the executable is `4cc857f0450fe5335454a7d727e9ba216fb6a352cd67161abeae120fa2bac09d`
and metallib is `3da3dbc1cc1b2bf43df33ed87f4ef9c9f8b15d234a64fcbd3a0508ff143d76ca`.
These artifacts, their 183 source files and 13 host archives are retained together
for the fresh Pavilion runs. Their combined fixed-seed quality result remains
separate from the passing regression suites.

Published `337c4fd` subsequently adds explicit empty values and a zero guard count
to the disabled Metal shading-normal probe stub. This fixes Linux Clang's
missing-field-initializer error. The stub is excluded from the native macOS
backend, and native transport code is unchanged.
The frozen rendering snapshot remains `c3489bf`, including its historical stub;
its source and binary hashes above are unchanged by the later publication commit.
The mandatory pre-push Debug run passes all 220 tests, retained under
`.cache/pavilion-convergence/final-v8/publication-gate-001/`. A separate isolated
upstream Clang 18.1.8 Debug build with Metal disabled passes 158 tests, strict
syntax checks on all four stubs, formatting and focused Clang-Tidy; receipts are
under `final-v8/ci-portability-001/`. That local check runs on macOS, not Linux.
All six jobs in [CI run 44632608875](https://github.com/rlefko/rypath/actions/runs/44632608875)
pass for `337c4fd`, including Linux and macOS builds, repository policy,
CodeQL, Clang-Tidy and the foundation gate.

The frozen candidate also passes the fixed day repeatability check: four fresh
processes and acceleration structures render eight owners at all 256 absolute
sample indices. All 8,192 packets validate; every 72-byte public packet record,
216-byte owner reduction and decoded summary agrees exactly across the four
contexts. The comparison authenticates each process's command and output files;
eight independent negative receipt cases are rejected. This covers the fixed
owners involved in the earlier instability, not universal full-image repeatability.

Fresh 320×170 seed-zero pilots complete at day 256 spp and night 512 spp, with
finite raw and authored-clamped FLOAT planes. Render subprocess times are
300.995 and 1,026.314 seconds respectively; the historical renderer-17 pilots
took 122.118 and 537.933 seconds. These are incidental observations across
revisions. The completed separate warm-up and repeated timing comparison is
reported below.
The day comparison changes 428 of 54,400 linear pixels relative to the same-seed
renderer-17 pilot; only 77 pixels change after standard sRGB conversion. These
combined-revision differences do not attribute individual paths or establish
variance parity.

The fresh full-image and 48-endpoint study uses the same frozen candidate and
unchanged seeds, sample budgets, regions and thresholds. A separately bound
operational supplement derives full-render deadlines from the completed pilots:
`ceil(pilot_seconds * 25 * 2 + 600)`, with an 18,000-second minimum and an
86,400-second ceiling. This retains the day deadline at 18,000 seconds and extends
night to 51,916 seconds. Commands, estimator settings and the 300-second completed
export deadline remain unchanged. Each invocation records its budget and deadline
before execution; failures and interrupted attempts remain preserved.

Both fresh 1600×850 images are now complete and independently audited against
the frozen renderer-18 artifacts above, with sampler and material seed zero:

| Scene | Samples per pixel | Total camera samples | Render subprocess seconds |
| --- | ---: | ---: | ---: |
| Day | 256 | 348,160,000 | 7,834.94 |
| Night | 512 | 696,320,000 | 26,429.18 |

Every pixel has the expected uniform count. All checkpoint accumulation planes
and compensation pairs are finite. Each raw and authored-clamped RGB FLOAT EXR
has 4,080,000 finite components and reconstructs exactly from compensated camera
and splat sums: maximum error is 0 ULP in both planes for both images. All
4,080,000 PNG channels per image also match independently reconstructed native
sRGB bytes exactly. Raw output is unexposed; the clamped Film sums already carry
authored ISO exposure (day 5, night 2.5). Completed checkpoint export preserves
the durable checkpoint. The unchanged 159-asset closure and authored Film settings
are authenticated by the audit.

Render receipts and PNG/raw EXR/clamped EXR/checkpoints are retained under
`.cache/pavilion-convergence/candidate-v8-full/`; independent results are in
`final-v8/full-artifact-audit/day-audit.json` and `night-audit.json`.
The SHA-256 values below agree between those completed audits and the day/night
render receipts. They identify the frozen `c3489bf` renderer-18 full outputs,
separately from earlier renderer-14/17 images and the later stub-only commit.

| Artifact | SHA-256 |
| --- | --- |
| `day.png` | `1f4953c7cbe529b000b67c236d9142b855d21651dd3f54527a76926e0b28ccd3` |
| `day-raw.exr` | `12652bb35c7f2fdf848436ed8dde0560d20ddd9d99408becf15f3f0854559605` |
| `day-clamped.exr` | `beff36133c357dd1a8616a92d98405770a6f53e9306325758478cb80565f7389` |
| `day.rycp` | `e219bdcb435e771de022ed22d35c2f878d612fcd91d83a524b1eca24d8fac9ae` |
| `night.png` | `87dc26815af15e7ec35ba019bdfac668bd08a541b4f5a5ccde9391ce91789251` |
| `night-raw.exr` | `bc5b19cef398393925533bdd7d2195a50cff32d96902e92793179e5e9f3719eb` |
| `night-clamped.exr` | `8f9734ad3e880b46dfb00a0b8528bcb91b6a1104ee0020da352fb1f57471aba8` |
| `night.rycp` | `edefa32ae891136b137cda4b9aa378eba055152c4c8e860c94c4509764b88f3a` |

These render times are incidental completion observations, not repeated
performance measurements. Artifact validity makes no historical pixel-identity
or convergence claim.

## Completed renderer-18 quality and timing study

The full fixed study is complete and failed. All 192 primary records and 384
FLOAT EXRs are present: 64 fresh v8 candidate records and the unchanged 128
PBRT/baseline references. None of the 48 nominal simultaneous quality bounds
passes the unchanged 1.25 ratio limit. Raw mean safeguards pass 26/36: day 18/18,
night 8/18. PR #51 remains a draft for diagnosis of the new night mean failures
and remaining outliers; passing native regressions and full-image audits do not
satisfy this scene-level target. Historical failed studies remain unchanged.

Largest remaining point ratios against PBRT are:

| Dataset / region | Variance ratio | Tail ratio |
| --- | ---: | ---: |
| Day raw / soffit | 218.00 | 7,879.96 |
| Night raw / floor | 41.40 | 44.50 |
| Day raw / wall | 2.20 | 19.09 |

These point estimates are distinct from the simultaneous upper bounds. The ten
failed raw night mean safeguards have the following relative shifts from PBRT:

| Region / failed channels | R shift | G shift | B shift |
| --- | ---: | ---: | ---: |
| Full / RGB | −5.44% | −8.63% | −6.34% |
| Soffit / R | +3.41% | passes | passes |
| Floor / RGB | −32.65% | −33.84% | −37.46% |
| Glass / RGB | −17.08% | −15.81% | −13.06% |

These are observed mean differences, not an attribution to a particular code
change or proof of estimator bias. The unchanged finite 32-replicate bootstrap
uses 20,000 joint draws across all 48 endpoints. Independent review authenticates
the native inputs, recomputes point statistics and mean safeguards, and checks
reported-bound arithmetic and bootstrap semantics; it does not regenerate the
20,000 draw maxima. The nominal one-sided 95% simultaneous limits are not a
coverage theorem for these heavy-tailed data, and the 36 mean safeguards are not
36 simultaneous confidence intervals.

Separate whole-process timing at 320×170, 64 spp and seed/material zero uses one
warm-up followed by all three measured runs per scene/version:

| Scene | Frozen v5 renderer 14 seconds | V8 renderer 18 seconds | V8 / v5 |
| --- | ---: | ---: | ---: |
| Day | 52.063 ± 0.131 | 117.714 ± 0.293 | 2.261× |
| Night | 57.946 ± 0.104 | 171.566 ± 0.914 | 2.961× |

The ± values are sample standard deviations, not confidence intervals. Both
versions use sampling contract 7. The comparator is the retained v5 candidate,
not the merged-main baseline. This fixed workload establishes a measured
slowdown; it is not a time-to-quality comparison or a primary-quality runtime.

An audit of the same twelve measured logs isolates a larger regression inside
the existing render-and-accumulation timer:

| Scene | V5 render loop seconds | V8 render loop seconds | V8 / v5 |
| --- | ---: | ---: | ---: |
| Day | 7.072 ± 0.017 | 72.387 ± 0.175 | 10.236× |
| Night | 13.074 ± 0.019 | 126.309 ± 0.490 | 9.661× |

This is a host wall-clock interval, including GPU waits, packet validation and
decoding, Film accumulation, progress and checkpoint callbacks. It is not a
GPU-only measurement. Its increase accounts for more than 99% of the observed
whole-process increase. Roughly 43 seconds outside the named phase timers
dilutes the whole-process ratios; that residual is not identified as pure I/O.
V8 also writes one additional intermediate checkpoint in each measured run.
Performance recovery and quality at equal elapsed time remain delivery
requirements.

A subsequent day-64 diagnostic replaces only host timing code and retains the
frozen metallib. Its unmodified-host control and instrumented host both reproduce
the original scene identity, all 11,750,400 Film payload bytes and all raw/clamped
FLOAT components exactly. All 896 completed command-buffer GPU timestamps are
valid: their intervals total 71.197 seconds within a 72.965-second render loop
(97.6%). Host setup/encoding takes 0.725 seconds, decoding 0.506 seconds, and
Film add/commit 0.083 seconds. Three checkpoint writes total 0.126 seconds,
including the initial pre-loop save. GPU intervals overlap host dispatch waits;
these nested timers must not be added together. This diagnostic establishes
GPU execution as the dominant current cost, not a separately measured v5/v8
GPU-throughput ratio or attribution to one shader function.

A cache-only certified-quotient shortcut preserves the same complete Film and
image bytes but changes measured GPU time only from 71.197 to 71.105 seconds in
one matched pair. That negligible difference is insufficient evidence for a
performance fix, and the shortcut is not adopted. Its longer whole-process
runtime also includes first-use pipeline compilation.

A separate fixed eight-sample day prefix counts 1,873,243 closest-hit queries
and 14,932,069 software triangle solves: 7.97 candidates per query. Of those
solves, 70.44% return a fast-path miss, and none enter the extended-range triangle
fallback. This does not exclude the extended arithmetic used for pruning caps.
There are 2,043,662 cap attempts and 2,037,171 traversal resets; the current-best
triangle is offered again 2,037,171 times, or 13.64% of all solves. Every paired
original/instrumented workspace byte agrees after removing the counter payload,
including all 435,200 returned packets. These are instrumented visitation counts,
not proof of the uninstrumented candidate order or a whole-frame/night profile.

The following isolated experiments each preserve the fixed day-64 Film payload,
scene identity and every raw/clamped FLOAT component:

| Change relative to original v8 | GPU seconds | Change from 71.197-second control |
| --- | ---: | ---: |
| Certified normal-domain FMA product-residual check | 67.840 | −4.71% |
| Reuse the current-best triangle solve when offered again, retaining alpha calls | 69.839 | −1.91% |
| Guarded binary32 construction of the identical extended world enclosure | 66.041 | −7.24% |
| Exact normal-domain exponent scaling and extraction | 68.559 | −3.71% |
| Existing any-hit visibility for scenes without pass-through surfaces | 53.324 | −25.10% |

Each row is one exploratory observation against the earlier diagnostic control,
not a repeated performance result. The changes are tested separately and their
percentages must not be added. The enclosure experiment retains the original
integer fallback and ray slab/cap operations; optimized and UBSan checks compare
1,001,156 scalar cases and 100,102 boxes without a mismatch. Its run writes two
checkpoints versus the control's three; GPU intervals exclude those writes.
None of these results resolves the regression or establishes time to raw-image
quality. These measurements exercise isolated copies of v8; combined integration
needs separate validation. The normal-exponent shortcut also passes 39,948 native
old/new helper comparisons with intact guards and input bytes. The visibility
trial confirms that the prepared scene disables pass-through handling; it changes
candidate visitation under the existing Boolean occlusion contract, without
changing the sampled segment or alpha decision.

A separate host-only 128-thread pipeline cap gives 67.298 GPU seconds with the
same Film and image bits. This resource-policy experiment remains separate from
the integration. It combines compilation and dispatch-width changes; neither its
timing nor its reported pipeline limits establish register spills or occupancy.

The five arithmetic/reuse/visibility changes are integrated in `d1d205e`; the
quotient and 128-thread experiments remain separate. These changes preserve the
existing estimator and numerical fallback policies. Release and ASan/UBSan each
pass all 220 tests, and the portable Clang 18 build passes 158. Debug's first
219/220 run exposes missing texture-frame metadata in the new alpha fixture;
the corrected fixture passes its rerun, followed by all 64 native tests with
Metal API validation and the complete 220-test pre-push Debug suite. Focused
Clang-Tidy and Apple analyzer checks pass. Publication revision `b0b7bbb` passes
all six jobs in [CI run 44830941172](https://github.com/rlefko/rypath/actions/runs/44830941172).

The combined comparison uses the same M4 Max, 320×170, 64 spp, seed/material
zero and authored scene settings. Two production-CLI controls precede a warm-up
pair and three measured pairs per scene, alternating version order. The baseline
uses the frozen v8 shader and observed host; the current arm uses the integrated
shader and matching observed host. All 18 runs reproduce the original scene
identity, every 11,750,400-byte Film payload and all raw/clamped FLOAT components
exactly. Each observed run has 896 valid GPU timestamps, no dispatch errors and
the same request counts and hash. Metal API validation is disabled for timing;
no other renderer, test, compiler or profiler runs concurrently.

| Scene | Baseline GPU seconds | Current GPU seconds | Baseline whole seconds | Current whole seconds |
| --- | ---: | ---: | ---: | ---: |
| Day | 71.070 ± 0.030 | 44.788 ± 0.037 | 118.664 ± 0.012 | 92.212 ± 0.069 |
| Night | 124.990 ± 0.005 | 68.651 ± 0.020 | 172.559 ± 0.102 | 116.186 ± 0.252 |

Values are means and sample standard deviations over all three measured runs.
GPU time decreases by 37.0% by day and 45.1% by night; whole-process time decreases
by 22.3% and 32.7%. This recovers part of the regression without changing these
images. It neither measures time to raw-image quality nor closes the earlier
renderer-14 performance gap. Renderer-14 GPU timestamps were not captured, so
its historical render-loop times must not be used as GPU-only comparators.
The first attempt, `combined-performance-001`, failed before rendering because
the copied CLI lacked executable permission. Its evidence is preserved; the
completed `002` attempt fixes permissions without changing executable bytes,
workloads or ordering. The current binary's embedded source revision predates
the source commits; its source bindings match `d1d205e`, while `b0b7bbb` adds
documentation only.

A subsequent fixed two-run experiment applies only `[[clang::noinline]]` to
the outer validated traversal routine. Both day/night Film and FLOAT controls
remain exact. Its individual GPU times are 44.740 and 68.665 seconds, compared
with the integrated three-run means of 44.788 and 68.651 above. This supplies
no useful performance improvement and is not adopted. These single exploratory
observations do not measure occupancy or register spilling.

A native-triangle hint experiment uses opaque hardware candidates to seed the
precise closest-hit search and certify early shadow occlusion. Original-source
intersection validation remains authoritative; native distances and barycentrics
are not accepted directly. A fixed six-run exploration (off, closest, both for
each scene) preserves every Film byte, scene identity and raw/clamped FLOAT value.
Day GPU times are 44.931, 36.547 and 35.798 seconds; night times are 68.876,
58.218 and 52.271 seconds. Each run has 896 valid GPU intervals and no dispatch
errors. These are single observations, not repeated performance evidence or an
adopted implementation. The additional structure uses about 1.127 GB, with
70.742 MB peak scratch and 29.611 MB peak input buffers. Parse-dominated process
RSS does not measure incremental device-memory cost. Day's first whole-process
time is about eight seconds above the earlier baseline despite similar GPU time;
startup variability prevents attributing the full wall-time difference to hints.

The hint regression tests exposed a separate full-sphere pole defect with hints
disabled. The unchanged analytic helper rejects both roots after radius
reprojection rounds a pole one binary32 step beyond the implicit sphere bound.
A separate native-offer witness confirms the sphere is offered to the helper;
this is not a missing acceleration-structure candidate. Renderer 19 applies only
authored partial-sphere z clipping, following the active-bound condition in
[PBRT's sphere intersection](https://pbr-book.org/4ed/Shapes/Spheres).
Cylinder clipping, root arithmetic and UV bounds retain their previous behavior.
The new test fails on the old shader and passes with the repair. It covers both
poles, two radii, inside/non-unit rays, farther roots, hemispheres, clipped bands
and a translated sphere preceding a triangle, checking closest-hit and occlusion
against independent axial distances. Checkpoint readers reject renderer 18;
independent Python serializers reproduce the old goldens before deriving the
renderer-19 identity and checksum.
All 17 focused analytic/checkpoint checks pass in Debug, Release and ASan/UBSan
with Metal API validation; the Python checkpoint decoder passes 12 tests.

The follow-up fixed night seed-0, 512-spp control keeps the frozen host and all
other shader code unchanged while disabling the two importance shading-normal
correction applications. The rebuilt enabled control reproduces every raw and
clamped FLOAT component exactly. Disabling the correction restores 99.42–99.94%
of the raw floor RGB shift and 99.99–100.27% of the raw glass shift between v5
and v8 in this seed. This localizes those changes to the combined correction;
it does not establish which transport convention is physically correct or
replace the failed 32-replicate study. Both control runs take about 17 minutes,
so disabling the correction does not recover the earlier speed in this
experiment. These two incidental runtimes are not formal performance repeats.

The subsequent fixed PT diagnostic renders the preselected night glass and floor
regions at 320×170 and 512 spp, with 16 rypath seeds (0–15) and 16 PBRT seeds
(32–47). It retains original full-frame camera/sample coordinates. A same-AS
control exactly matches selected packet and accumulator fields against full-frame
execution; PBRT crop controls match the corresponding pixel bits. All planned
runs complete. The analysis compares these new PT outputs with the same seeds
from the retained BDPT study, using raw, exposure-normalized RGB:

| Comparison | Glass RGB mean difference | Floor RGB mean difference | Inherited mean checks |
| --- | --- | --- | ---: |
| Rypath PT / PBRT PT − 1 | −2.68%, −2.09%, −1.75% | +9.67%, +6.50%, +4.06% | 6/6 |
| Rypath BDPT / rypath PT − 1 | −0.61%, −0.69%, −1.08% | +38.22%, +31.94%, +14.46% | 4/6 |
| PBRT BDPT / PBRT PT − 1 | +18.26%, +16.98%, +13.96% | +144.21%, +129.40%, +97.17% | 0/6 |

The rule remains five combined standard errors plus
`2e-5 * max(1, abs(reference_mean))`; all 18 raw checks are retained. Rypath's
remaining within-renderer failures are floor red and green. Independent decoding
reproduces every seed mean and gate. Authored-clamped images are descriptive only:
their exposure is 2.5, whereas the raw comparison removes exposure. No clamped
acceptance rule or additional seed selection is introduced.

These finite, heavy-tailed samples do not establish PT equivalence or convergence,
identify an estimator bias, or determine the correct shading-normal convention.
They show that PBRT's brighter BDPT estimate also differs from its own PT estimate
in these regions. Matching that brightness alone would therefore be insufficient
justification for removing the correction. The remaining floor discrepancy needs
contribution/strategy attribution; this diagnostic does not replace the failed
32-replicate quality study.

A frozen-v8 replay also rechecks the preselected day outlier: seed/material 25,
pixel (119,48), sample 171 of 256. All 256 owner contributions reconstruct the
retained raw RGB exactly, and eight original/instrumented requests retain their
entire original workspace prefixes. The initial camera audit used the scalar
lens-probe sampling route and failed. A separate one-dispatch control follows
the actual packet lens-dimension override and exactly reproduces the captured
film words, lens words and primary ray. Its corrected offline admission retains
the failed original audit and all other gates, without rerendering the owners.

The admitted trace preserves the historical diffuse, dielectric transmission,
coated reflection, dielectric transmission and environment path. Only the
`(s=0,t=6)` connection contributes, with MIS weight one; the `4/9` and `9/4`
transmission factors cancel. Sample 171 supplies 99.8881% of this owner's
weighted luminance. The final spectral contribution differs from the historical
v4 trace by at most 1.812e-6 relatively. This is the persistent rare refracted
environment contribution, with no demonstrated MIS or refraction-scaling
amplification defect. The importance correction does not execute on this
radiance walk. Independent exact-rational checks validate all four selected
triangle hits and their nearest ordering within each selected instance; all
twelve returned distance/barycentric floats round correctly. These checks do
not establish scene-wide closest-hit ordering, visibility, population variance
or the cause of the full day/night quality gap.

A subsequent fixed diagnostic compares polar and concentric diffuse-direction
mapping under Halton and independent sampling at that same preselected owner.
All four conditions retain the original camera/raster context, 32 seed/material
seeds and 256 spp. The original Halton/polar condition first reproduces all 32
retained final raw/clamped pixels exactly. All 128 cases then complete with
32,768 correctly identified, guarded packet records and zero splats. Day's
`volpath` route globally disables light-path splats, so the selected owner's
reduction is complete; the other 54,399 pixels are explicitly unrendered.

| Mapping / sampler | Raw pixel luminance mean | Variance across 32 seed estimates | High-contribution samples / 8,192 |
| --- | ---: | ---: | ---: |
| Polar / Halton | 0.136588 | 0.556373 | 1 |
| Polar / independent | 0.005083 | 8.682e-7 | 0 |
| Concentric / Halton | 0.004945 | 7.031e-7 | 0 |
| Concentric / independent | 0.004995 | 6.035e-7 | 0 |

The preset high-contribution threshold is weighted, unexposed sensor Y greater
than one; this is not a classification of every caustic path. Its sole event is
the original seed-25/sample-171 contribution. The retained PBRT day runs at
seeds 32–63 give a contextual pixel mean of 0.004773 and variance 1.970e-6 after
removing exposure; these are different seeds, not paired candidate controls.
Independent auditing verifies packet identity, sensor sums, Film reduction,
output conversion, seed statistics and all four paired one-factor contrasts.

The outlier disappears from this small dataset when either the mapping or the
sampler changes. Because the owner was selected for its original outlier and
independent sampling also removes that observed event, this does not isolate a
mapping-specific improvement or establish lower population variance, equal
support, or PBRT convergence parity. No sampling change is adopted from this
result. A predeclared 4,096-spp extension is not run: before variant outcomes,
its forecast is 20.440 seconds of setup plus 64 times the 27.302-second baseline
owner loop, exceeding its 600-second operational budget. The original failed
scene-level quality verdict and thresholds remain unchanged.

A subsequent host-only batching diagnostic uses eight independent owner lanes
per dispatch with the same frozen shaders. Independent auditing reproduces all
128 earlier cases: 32,768 complete packet records, every Film byte and all 768
raw/clamped RGB values are exact. The sole wire-parameter difference is lane
count one to eight; sample order and eight-sample Film commits remain unchanged.
The larger extension is again declined before extended outcomes: 20.019 seconds
of setup plus 16 times the 41.007-second complete four-arm control loop predicts
676.126 seconds, above the unchanged 600-second limit. No 4,096-spp or new tail
observations follow from this run; the original cost decision remains retained.

A separate diagnostic adds the same importance correction to pinned PBRT. Its
uncorrected seed-32 control matches the retained FLOAT image exactly, but the
corrected full-frame run stops at a guarded negative contribution. Fixed replays
at pixel (46,64), sample 86 isolate `(s=1,t=6)`: a one-sided spherical emitter's
`PDF_Le` returns -0.0004199222 for a back-hemisphere direction. Conversion to area
density gives -15.30755; the MIS recurrence then returns -0.05610157. The four
pre-MIS spectral lanes are positive. Both corrected and uncorrected diagnostic
binaries reproduce every captured recurrence bit; neither applies the importance
correction in this sample. This attributes the guarded failure to the pinned
reference's reverse emission PDF, not to the correction. Rypath already returns
zero outside the one-sided emission hemisphere. The failed diagnostic remains
failed; no corrected image or statistical conclusion follows from those witnesses.

A subsequent fixed diagnostic repairs that hemisphere support in both PBRT arms
before comparing the importance correction. Both modified binaries pass the
same 3,089 angular queries and the captured failing path. The new study completes
all 32 full raw night renders: seeds 32–47, 320×170, 512 spp and maximum depth
five, with correction disabled then enabled for each seed. These are modified
PBRT diagnostics, not official reference images or fully matched floating-point
implementations. The inherited regional mean rule is unchanged: five combined
standard errors plus `2e-5 * max(1, abs(reference mean))`.

| Region | Correction-enabled PBRT RGB mean | rypath BDPT RGB mean | Checks within inherited rule |
| --- | --- | --- | --- |
| Glass | 0.076464 / 0.043620 / 0.020216 | 0.073898 / 0.042224 / 0.019496 | 3/3 |
| Floor | 0.016633 / 0.006792 / 0.001913 | 0.016675 / 0.006836 / 0.001930 | 3/3 |

The correction lowers PBRT's glass means by 11.6–15.4% and floor means by
38.1–40.1% relative to its shared-support, correction-disabled arm. The support
repair alone leaves every floor mean exactly unchanged; its largest glass mean
change is 1.95e-9. All twelve paired contrasts retain their seed differences,
covariances and descriptive standard errors. This isolates the correction as a
substantial cause of the prior cross-renderer brightness difference in these
regions. It does not establish variance parity or a converged reference.

Independent auditing recomputes all 32 new and 48 retained reference images.
All eighteen inherited comparisons remain visible: correction-enabled PBRT
passes 6/6 against rypath BDPT, 4/6 against PBRT PT, and 0/6 against original
PBRT BDPT. Floor red and green remain higher than PBRT PT by 51.2% and 39.6%;
blue is 18.0% higher but inside the rule. The remaining BDPT/PT discrepancy
requires separate convergence or path-level evidence. The existing finite coated
model is one concrete hypothesis: joint-walk continuation and independently
evaluated directional scattering are distinct approximate routes (see the
[Pavilion coating record](pavilion-validation.md)). This is not yet an attribution
of the floor difference. Matched outer depth and compensated path-tracing roulette
do not identify a deterministic termination defect. This diagnostic does not
replace the failed scene-level quality study.

A bounded material diagnostic decodes a retained horizontal coated-floor vertex
(sample 118, camera vertex 3), then evaluates its exact outgoing direction and
four wavelength/material lanes through the pinned PBRT oracle. The interface is
rough (`alpha = 0.102019608`), so this case has no omitted specular atom. It fixes
32 replicates of 4,096 queries per lane: 524,288 total, with every zero return
retained in its denominator. Independent auditing regenerates all input rows and
recomputes the five moments and paired standard errors.

| Lane | Joint-walk constant-light mean | Directional evaluation mean | Difference ± paired SE |
| --- | --- | --- | --- |
| 0 | 0.09582243 | 0.09696519 | 0.00114276 ± 0.00054902 |
| 1 | 0.07554390 | 0.07580426 | 0.00026036 ± 0.00052064 |
| 2 | 0.12259998 | 0.12487037 | 0.00227039 ± 0.00060237 |
| 3 | 0.10820541 | 0.10983460 | 0.00162919 ± 0.00056007 |

These descriptive constant-light differences are 0.34–1.85%, with the largest
3.77 paired standard errors from zero; there is no new acceptance cutoff. A
second smooth directional lighting function also remains in the retained results.
The lanes share variates, and one outgoing direction under two simple lighting
functions does not bound localized differences under Pavilion illumination or
MIS. This does not explain or dismiss the scene-level floor discrepancy.

The directional PDF proxy integrates to 2.356579 ± 0.007031 SE in this case.
That is not by itself evidence of bias: source inspection confirms both pinned
PBRT BDPT and rypath keep the joint sampled throughput weight separate from the
forward/reverse directional densities used for MIS. Neither divides sampled
throughput by this proxy. Conditional agreement of the two scattering routes and
consistent strategy weights remain open questions; substituting the joint PDF
for the directional proxy would mix different probability measures.

The frozen native-triangle prototype subsequently completes a fixed 16-run
comparison: one warm-up per mode and three alternating measured pairs for each
Pavilion scene, at 320×170, 64 spp and seed/material seed zero. Every full Film,
identity, request sequence and raw/authored-clamped FLOAT component matches its
control. All 14,336 GPU intervals are valid. Independent auditing reproduces the
following means and sample standard deviations (three measurements per arm):

| Scene | GPU seconds, disabled → enabled | Whole-process seconds, disabled → enabled |
| --- | --- | --- |
| Day | 44.923 ± 0.035 → 35.785 ± 0.037 | 91.954 ± 0.058 → 82.924 ± 0.103 |
| Night | 68.840 ± 0.040 → 52.146 ± 0.010 | 116.010 ± 0.085 → 99.329 ± 0.085 |

GPU time falls by 20.3% and 24.3%, respectively. Additional acceleration storage
is 1,127,115,776 bytes by day and 1,127,531,520 bytes by night; peak construction
scratch is 70,741,632 bytes and input staging is 29,611,248 bytes. The already
shared primitive records are not counted again as a new allocation. About
43 seconds of whole-process time remains outside the classified phases; it
cannot all be attributed to renderer preparation. These are matched-workload
results on the frozen renderer-18 prototype, not measurements of current
production code or time to a shared quality target.

A separate fixed 32-run comparison exercises opaque hit/miss geometry, a scene
containing accepting and rejecting alpha layers before opaque geometry,
all-alpha geometry, and an analytic-only scene. Each uses 64×64, 64 spp,
a mode-zero/mode-three warm-up,
and three alternating pairs. All 32 full Film/identity controls, 64 FLOAT planes
and 2,048 GPU intervals pass; ineligible scenes allocate no hint resources.
Enabled opaque GPU means are 0.0753 → 0.0721 seconds and mixed-alpha means are
0.3428 → 0.3311 seconds. Disabled controls also vary by a few milliseconds, so
these small timings do not establish a general speedup. They provide bounded
correctness, routing and resource evidence for a production integration attempt.
The subsequent renderer-20 integration passes the current-source geometry,
persistence and scene controls described below.

Normalized forward-transform products provide a separate execution experiment.
The first shortcut retains the general arithmetic guards and renormalization.
It passes strict numerical checks, but its single exploratory day/night pairs
increase GPU time by 4.5% and 4.9%; it is not adopted. The leaner version uses a
rounded multiply plus FMA residual only for two exponent-127 operands, retaining
the general fallback and both-limb rescaling checks. Its strict geometry and
render-`-Os` probes pass 121,186 cases, including incoming-invalid state, against
both the old graph and an independent integer oracle.

The leaner version's separate current-renderer-19 pilot preserves all four full
Film/identity controls, eight FLOAT planes and 3,584 valid GPU intervals. Day GPU
time is 44.765 → 40.688 seconds and night is 68.662 → 62.589 seconds. Whole-process
day time nevertheless rises from 90.46 to 97.60 seconds; night falls from 115.27
to 109.48 seconds. These single unwarmed pairs motivate a fixed repeated study,
not a general speed claim. Their controls do not measure the combined current
renderer-20 integration. The old slower variant and adverse day wall time remain
part of the evidence.

The fixed follow-up completes sixteen runs: one off/on warm-up and three measured
pairs in alternating order per scene. Independent auditing verifies every full
Film/identity record, 32 FLOAT planes and 14,336 valid GPU intervals. Measured
means and sample standard deviations exclude warm-ups:

| Scene | GPU seconds, old → specialized | Whole-process seconds, old → specialized |
| --- | --- | --- |
| Day | 44.781 ± 0.037 → 40.693 ± 0.034 | 91.270 ± 0.255 → 87.017 ± 0.150 |
| Night | 68.648 ± 0.038 → 62.567 ± 0.042 | 115.873 ± 0.169 → 109.580 ± 0.159 |

GPU time falls by 9.13% and 8.86%; whole-process time by 4.66% and 5.43%.
Peak resident memory remains about 13.78 GB in each arm. These are frozen
renderer-19, matched-workload measurements of the arithmetic specialization,
not combined renderer-20 measurements or time to a shared quality target.
The native-hint and arithmetic speedups must not simply be added together.

The current renderer-20 integration passes all 222 ASan/UBSan tests and 159
portable tests. Release validation consists of 217 checks with Metal API
validation enabled plus the five CLI checks in their default environment. The
first full API-enabled Release run retains five failures: those commands exit
successfully, but Metal's injected banner violates their unchanged empty-stderr
assertions. No renderer or test expectation is relaxed. Static analysis of both
production Metal adapters and the native probe passes. Focused Clang-Tidy first
reports integer widening in a test's expected allocation size; explicit operand
types fix it, with that test rerun successfully in all four builds. The Python
checkpoint decoder passes twelve tests, and both older-host/newer-library and
newer-host/older-library combinations reject their renderer-contract mismatch.

Fresh renderer-20 day and night renders at 320×170, 64 spp and seed zero run with
Metal API validation. Each full 11,750,400-byte Film and all 652,800 combined raw
and authored-clamped FLOAT components match renderer 19 exactly; identities
change as required by the contract. Independent auditing verifies the source and
asset bindings and reconstructs all 326,400 standard-sRGB preview channels.
These are correctness controls, not repeated combined-performance measurements.
Receipts under `.cache/validation/` retain the initial API-enabled failures
(`native-hint-001/current-validation.json`,
`797e0d7dd846c35b5a06ee1d4b4e666e0aa3a11492ab0ec473acf58e71994fb6`),
post-fix static and affected-test validation
(`b7011e9396c8e346890a48ab100fdfcd53299737bc02a309c991788722161e90`),
fresh scene controls
(`94244febd385ed5b492ef8b29fa0fb29896d8736b9b3959098b691371bccb1d1`)
and their independent audit
(`3e0a418638e7f7268da38884fb1680acb77e28c47978592593e98d01d17a4b55`).

Authenticated evidence under `.cache/pavilion-convergence/final-v8/`:

| Receipt | SHA-256 |
| --- | --- |
| `quality-metrics.json` | `a24b609a251382b9f87825125a536939b4fc131f9c034c407f2e20071c70a93a` |
| `independent-final-review-001/audit-result.json` | `719b7aa52edc2e99feef24d9f9f40da888a6251f96db5a88edaf98514c36db3e` |
| `report-plots-001/outputs/receipt.json` | `99938f18a0e9f40b5f3ccd3e7de701a0187f57f74786032efbbba473d19bda95` |
| `performance-diagnosis-001/phases.json` | `bc050cd6d022b1abcd2d335a050d6e544242008abb35a830cc2ccaf75a1ef5c5` |
| `performance-review-001/review.json` | `2e39897964b90cb0848da6de2551c022a40501a6e001996c0bcdb61237e997dc` |
| `host-timing-proposal-001/diagnostic-execution.json` | `dd7792b34bb25395f48adb53694c9474fb660f3a3360ca143616b9fb535bdec1` |
| `traversal-counters-001/seal.json` | `46824b424ff1b1296b56c57bea869cf406b60ece85b6507ae859358ef1ba0ade` |
| `fma-guard-prototype-001/result-interpretation.json` | `8ea05a96e9f62c2dbb254428a431a02a6cc00ec07c307a4e0e2290707db75f8e` |
| `best-triangle-cache-001/audit.json` | `a71f57e85d8e735875ebdcd806f39b198ebd43b7b644747920a91100024c0921` |
| `float-bounds-prototype-001/result-interpretation002.json` | `5f6ec59e625597a79e106802075c1ecce5f328ceed108131765a04babb46291e` |
| `visibility-anyhit-prototype-001/audit.json` | `a0b91ee48c2259e446d4b8143f741f98be2517c54fc021e130de268dc3add9d6` |
| `normal-exponents-prototype-001/result-interpretation.json` | `ea32555658271ce4c0dad34af3587087d56fbd6bdc664fed2e7720184399efd2` |
| `pipeline-width-prototype-001/root-execution.json` | `b38bf53eeffa46f21c6a0f9c7759061ffd81a9b905672e5aacc8af1bb9497baa` |
| `combined-performance-002/execution.json` | `c8e9a0046ed8d3fff74dc21b5306e97dfda302a33fe611a395889ca9e233e6ec` |
| `combined-performance-002/independent-review-001/receipt.json` | `b6842f8817b6876c5ad65ee782bfc959bfb748a6aff09225f8a6d98264b9020a` |
| `traversal-noinline-001/execution.json` | `9c77863f81deaf658a123b5f6d0898731f9b1d85c82a60b54409ae5788e2528a` |
| `adjoint-control-001/interpretation.json` | `f1a2f57b09b70b499b13b38c4c89e79a40587257b11188bad6e6c3f8020a7db2` |
| `night-pt-roi-diagnostic-001/study-execution.json` | `f2a028ece2dfed74dac0913bdc90b1f0c25750025e1ffbbcc9d506e2e7c1be25` |
| `night-pt-roi-diagnostic-001/analysis.json` | `96ec126ce6383455368063cd3887208003b02faae00332e78276c0f421235572` |
| `day-owner-trace-001/offline-readmission-001/receipt.json` | `0a8d2b905f4176fc4483fb03d3683196e5fe93843bea059bdd548954f6e0d3fa` |
| `day-owner-trace-001/independent-path-review-001/receipt.json` | `fbf4227309933ed9a98b7e8da510d3f623b52dcd0fe3fdb1cac83c8bf0c50848` |
| `day-owner-trace-001/independent-path-review-001/selected-oracle-review.json` | `3244f5abb2b6dbf81f42433126860339cc258db7946a2e288da4ab2b499d76c1` |
| `native-triangle-hint-003/trial/execution.json` | `741c86c84c61aec1d76a410c39b2f6a74a33b232795867f75610036f54223367` |
| `native-triangle-hint-003/trial/independent-review-001/receipt.json` | `7f386f6b027d87902debe81891765af2ce6448ca7f07c4847b8cc8077a4b2699` |
| `native-triangle-hint-003/formal-comparison-001/execution.json` | `01e6fa65f7cc9f1a2eca34c073432ebd6658697351ed9231c86349c1e834cbc1` |
| `native-triangle-hint-003/formal-comparison-001/independent-review-001/receipt.json` | `95c83d8ba427fee97b0fe2afee620e0ce8fb1620c404b2674e41a4e5810e25eb` |
| `native-hint-small-scenes-001/execution.json` | `48f336baf8c3c48b62323a27747e71658cb1a0527c0be44c3629f019778c5f75` |
| `native-hint-small-scenes-001/independent-review-001/actual-review.json` | `1dfae3daf7b934f538a647f03118dbfa7157a5b4806994aa80c4413d7c5cfc72` |
| `pbrt-adjoint-control-001/failure-capture-002/execution.json` | `ce50f14eced7765bbeb265e0a5abb3e384bc20d4aaa5d091bcbaee714dd31bf7` |
| `floor-material-moment001/pdf-source-review001/REPORT.md` | `334d89f0aacd9ff0912f64b5f845aa5634889f6011e1a6c235c2df401d4bf28b` |
| `floor-material-moment001/moments001/execution.json` | `c51cd903a004ec9784b75b27f76c73f47a29d95a0776b5b6a6e12bce96168dc5` |
| `floor-material-moment001/moments001/independent-review-001/receipt.json` | `54707073fe083cfab866297646f469c1a589e5d658ec594ae547bed1cea0655e` |
| `pbrt-adjoint-control-001/shared-support-001/study-execution.json` | `dca429621fda6c13a68b6adcc916cdcae7001dae048ba38d669c38c2e20fb393` |
| `pbrt-adjoint-control-001/shared-support-001/study-analysis.json` | `a5206ae179de8578456f030d5690e2a374465e549c9e7b236b8f2e9f9a6c5839` |
| `pbrt-adjoint-control-001/shared-support-001/independent-review-001/study-review.json` | `76fae336c2d4d8748d9ab3f3684f3667dcbf7e4314124d61fa96a1b16cdfbf86` |
| `day-cosine-map-diagnostic-001/execution.json` | `9615f0bb4bd5f65bd9112e988e2e9c5e4b6689b8a7891d927d69246791659c81` |
| `day-cosine-map-batched-001/execution.json` | `8e44cb920b35f8fd72e9154dd79d1c21900dc8f997d8733fa862d2ac303446bc` |
| `day-cosine-map-batched-001/independent-review-001/actual-review.json` | `4c3729835fd923387b4df99d5ac5cc34b612cbcc94ff0e68b4add5635eee9684` |
| `normalized-transform-performance-001/execution.json` | `912493cfabeb1b08874e442132aeb3fd8394a78b1bcab537d0ee3c728404c8a6` |
| `normalized-transform-product-fast-001/execution.json` | `f0bca4790c1f8b6fcfd81b4bef672da410683490f9ae7bb56ca94cbaabd27238` |
| `normalized-transform-fast-performance-001/execution.json` | `d4806a3bdf49f205e3dda5a92d2520fcf985494543a475821fc4621416ca1011` |
| `normalized-transform-fast-repeats-001/execution.json` | `1fbce560a260f8eeafe937feb861b9cf3b07d956d4a3a510c8b64aa5046e9b59` |
| `normalized-transform-fast-repeats-001/independent-review-001/actual-review.json` | `3767c80a360155642a746a89c2f48c1c0531229a6c1da029a2c6355c24d738a0` |

The descriptive report's `report-plots-001/outputs/summary.json` retains all
endpoints, mean safeguards and timing runs. The
[sampling/convergence record](sampling-convergence-validation.md) provides the
complete study context; no seeds, thresholds or adverse endpoints were removed.

The integer rounding and error-free-transform design follows the numerical
principles described by [Berkeley SoftFloat](https://www.jhauser.us/arithmetic/SoftFloat.html)
and [Ogita, Rump and Oishi](https://www.tuhh.de/ti3/paper/rump/OgRuOi04a.pdf);
the bounded exponent-pair implementation and traversal policy are rypath adaptations.
