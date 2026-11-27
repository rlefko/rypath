# Enhanced ReSTIR development

The target is native 1920×1080, one fresh four-wavelength hero path per pixel,
Area ReSTIR with optional corrected MCMC, and denoising: **20 FPS for Cornell and
Prism, and at least 10 FPS for Pavilion day and night**. These are complete-pipeline
budgets of 50 ms and 100 ms per frame. Bare path-tracing throughput is diagnostic,
not the acceptance criterion. Report static and moving-camera results, frame-time
tails, startup, and image quality separately. Preserve authored scene semantics;
resolution reductions, model simplifications, radiance clamping, and biased reuse
are not implicit ways to satisfy these targets.

## Current implementation and paper coverage

The default Area mode is a full-replay GRIS adaptation. The opt-in Enhanced mode
adds the mechanisms below but does not yet reproduce every optimization in
[ReSTIR PT Enhanced](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/)
or all material/geometry cases in the original hybrid reconnection architecture.
The default summed-domain warm static frame performs six full path evaluations per pixel after exact temporal memoization:
one fresh evaluation, one mutation, and four spatial evaluations.

| Mechanism | Current coverage |
| --- | --- |
| Contribution-aware resampling MIS | Source/destination target adaptation |
| Confidence recurrence, §2.2 | Enhanced caps the historical input before MIS and sums structural spatial inputs. The default is a fixed cap of 20 |
| Reciprocal spatial work sharing, §3 | Opt-in per-pixel Gaussian pairing atlases and shared same-pair reverse targets |
| Final vector-weight shading, §6.3 | Conditional-mean readout, separate from the selected history sample |
| NEE candidate sampling, §§6.1 and 7 | Existing light proposals; the evaluation's 32/B² light-candidate schedule is not reproduced |
| Contribution-path reservoir, §§2.3 and 6.1 | Opt-in finite-depth `(U,j)` with fresh local selection; full or selected-prefix replay |
| Hybrid reconnection and Jacobian, Eq. 2 | Owned triangle/world-direction charts for admitted diffuse and rough-conductor roots; forced incoming/outgoing evaluation and scaled chart densities |
| Dual-footprint eligibility, Eq. 5 | Runtime first-eligible selection, forward/inverse footprints and rolling predecessor veto |
| Forced NEE endpoint reconnection, §6.2.3 | Enhanced owns triangle-area, positional/distant-mass and environment-direction endpoints; other shapes retain replay |
| Cached suffix evaluation | Bounded two-operation sequential hero tape; exact prefix replay on misses |
| Selective shift-pair compaction, §6.2.2 | Opt-in admission skips structurally selected-null requests; all nonnull requests retain canonical evaluation |
| Initial-only external roulette, §6.2.4 | Opt-in `enhanced`: independent discovery gates and canonical no-outer-roulette imports |
| Reservoir reprojection and dual motion, §6.4 | Opt-in camera-only dual motion; focused controls and four-scene native motion smoke pass |
| Duplication-based history cap, §5 | Optional initial-seed neighborhood cap with fractional temporal and spatial confidence. Other ancestry maps remain available for diagnosis |

Published performance is evidence about that implementation and hardware, not a
forecast for M4 Max or for our spectral/coated-material model.

## Implementation order and contracts

These staged contracts are retained for context; the runtime sections below
record the implemented mechanisms and their separate validation evidence.

1. Establish controlled long-history comparisons. The repeated native study has
   unresolved Pavilion differences, including differences between unchanged
   baseline processes. Use compensated traversal and, where needed, sessions
   sharing the same acceleration structure to separate traversal sensitivity from
   cache or transport changes. Do not turn an observed difference into an error
   tolerance after the fact.
2. Reduce transport shader storage with measured execution changes. The tested
   preparation/event split was slower and remains rejected for adoption. A new
   approach must reduce expensive shader storage and complete frame time while
   preserving transport; material grouping must earn its queue and memory costs.
3. Introduce a contribution-path representation with a proved local selection
   weight. On a fixed counting measure over contribution slots, selection with
   probability `target[j] / sum(target)` gives initial normalizer `sum(target)`;
   it is not a weight-one fresh sample. Preserve early emission, NEE technique
   MIS, hero likelihoods, collapse compensation, roulette and inactive-slot
   support. Verify the conversion independently before renderer integration.
4. Add invertible hybrid shifts and cached suffixes, with forward/reverse support
   checks and the actual Jacobian in resampling MIS. Add footprint eligibility,
   forced light reconnection and compaction of pairs that require replay. Start
   with explicit supported continuous charts; replay unsupported Dirac, coating
   and spectral cases without silently substituting a different model.
5. Integrate the corrected mutation with that representation. A wavelength change
   cannot reuse wavelength-dependent suffix radiance unchanged. Measure actual
   prefix/suffix work and retain the spectral color controls.
6. Optimize reconstruction and validate the complete pipeline at matched quality.
   Keep raw accumulation, denoised output and display transforms separate.

The finite-depth contribution representation is a foundation, not a claim that
reconnection or the complete Enhanced algorithm is present. External roulette
is available only in the separate Enhanced domain below.
Unbounded-depth support requires its own declared measure and sampling contract;
it must not be implemented by an unreported truncation.

## Contribution-path runtime

`--area-path-domain contribution` opts into a fixed counting measure over
`E_0..E_D` and `N_0..N_(D-1)`, with slots `E_d=2*d` and `N_d=2*d+1`. It currently
requires four-wavelength hero transport, packed one-event wavefront Area replay,
and finite authored depth 0 through 65. The default `summed` domain is retained.
Unsupported combinations fail explicitly.

Fresh paths evaluate the original full transport path and select an atom with
probability `t_j/S`, where `S=sum(t_j)`. The initial normalizer is `S`, not one or
`S/slot_count`. Inactive atoms retain the positive target floor. Imports, temporal
memoization, reverse targets and MCMC keep the same `j`; a wavelength mutation
fully reevaluates that atom. The fixed slot travels with its selected replay
payload and weight. Mode, depth, target scale and replay policy belong to
history/cache identity.

`--area-contribution-replay full|prefix` is available only in the contribution
domain; `full` remains the default oracle. Prefix replay retains all original
continuation, hero-collapse, roulette and history work through the selected
event. It evaluates only the requested endpoint or NEE term, then stops after
that event. Omitted or unreachable slots remain zero. Fresh local selection still
walks the full path and considers every slot. This reduces repeated transport;
it is not hybrid reconnection or a cached suffix.

The exact differential contract covers paths for which full replay succeeds.
Prefix replay can succeed when an unselected term, later transport, or the full
radiance sum would fail; those calculations are absent from the selected-atom
evaluation. Failures encountered before or during selected evaluation remain
failures. The control is explicit because this changes the execution failure
domain; it does not silently turn encountered errors into zero radiance.

Each atom retains the original technique MIS and four spectral likelihood
weights. Projection uses all four original sensor wavelength PDFs, including for
emission before a later hero collapse. Projecting individual atoms changes FLOAT
addition grouping relative to projecting the summed path, so summed-versus-atom
comparisons use physical and statistical checks rather than bitwise equality.
Cache and queue controls within one domain still require exact equality.

The CPU checks cover fixed-slot support, signed readout, fresh generalized MIS,
and nonuniform 131-slot selection. GPU controls cover positive depth-zero and
depth-65 endpoints, early emission, dispersive/coated materials, null/invisible
crossings, repeated sample labels, queue capacities and cached replay. A 32-seed
paired multileaf mean check uses one whole-frame difference per independent seed;
its five-standard-error gate is a bias smoke test, not proof of unbiasedness or
scene-quality qualification.

The selected-prefix differential checks 2,227 pretraced fixed-slot cases at
depths 0, 3 and 65, including 426 positive endpoints, 404 omitted-slot witnesses
and 274 explicit full-failure/prefix-success partitions. It uses a synthetic
sensor, disabled roulette, no AS traversal and zero NEE. Separate full-frame
controls cover positive NEE, coatings, dispersion, early emission, null/invisible
crossings and a procedural texture graph. All six fixtures preserve exact XYZ
across full/prefix replay, cache settings, queue capacities and repeated labels.
Image textures and emissive textures are not covered by that fixture set.

A short native-1080p screen (seed 51001, two warmup and three measured frames per
arm) uses hero-visible sampling, Area reuse, one spectral-refresh mutation and
signed temporal denoising. Full contribution replay measures 0.798 FPS on Cornell
and 0.960 FPS on Prism; selected-prefix replay measures 1.202 and 1.149 FPS.
A matching Pavilion day/night screen measures full replay at 0.311/0.288 FPS
and prefix replay at 0.421/0.419 FPS (1.35×/1.45×). Final raw and reconstructed
FLOAT images match bitwise in all four scenes. These ratios compare replay
policies within the new contribution domain;
they are not speedups over the merged summed-domain renderer and do not qualify
sustained performance or the new domain's scene-level quality.

## Canonical Enhanced domain

`--area-path-domain enhanced` is an opt-in estimator domain with the same finite
0–65 depth and hero/packed-one-event requirements. It is not yet the complete
paper implementation. Each canonical contribution omits outer path roulette;
coating's internal walk roulette remains part of the material estimator. Fresh
paths discover those contributions using independent roulette entropy. An
observed atom has selection score `alpha/a`, where `a` is its discovery
probability. Arrival emission uses the probability before the current gate;
NEE uses the probability after it. Killed future atoms are unobserved, not padded
with artificial black contributions. Imports and mutations evaluate the same
canonical atom without these discovery gates.

Exactly represented zero XYZ has zero target. All-zero discovery publishes an
explicit null reservoir with zero normalizer, not an invented black atom. Its
proposal domain remains structurally present in both sides of resampling MIS.
A zero destination target receives zero mapped score and complementary canonical
coverage. Null mutations are self transitions; invalid arithmetic still fails
the frame. Positive targets retain the existing magnitude/floor policy.

The actual gate is a 24-bit dyadic probability. The stored product of at most
64 probabilities is rounded FLOAT, with the stated `gamma64` relative error
bound; this is not an exact floating-point Horvitz–Thompson claim. A pre-draw
unit-survival choice prevents the product entering an unsupported range. It
never clips an observed weight. Exact temporal memoization requires identical
complete evaluator contexts and unchanged pixel ownership. Logical, physical,
cached and analytic-null evaluations are reported as disjoint categories.

Release validation on M4 Max for this stage passes the shared arithmetic,
context, CLI, queue, Enhanced rendering and selected-prefix controls. The GPU
prefix probe retains all 2,227 cases and adds 16 selected-event controls plus six
actual Enhanced discovery cases, including independent spectral projection and
a two-dispatch 104-byte state round trip. Queue tests cover poisoned null state,
capacity two/seven, refill, invalid tags and duplicate completion. Rendering
controls cover depth zero/65, coatings, dispersion, null transitions and full
versus prefix replay. The 32-seed depth-three closed diffuse cavity check compares
against both summed transport and independent CIE/analytic radiance; its
five-standard-error gate is a bias smoke test, not scene-quality qualification.

Before this stage, the merged PR #78 runtime (`5fb7b24`, merged as `5194f8d`) was
measured at native 1920×1080 with hero-visible transport, contribution-prefix
Area reuse, one spectral-refresh mutation and signed temporal denoising. One
static run per scene used eight warmup and 32 measured frames, seed 61002:

| Scene | Mean complete frame | FPS |
| --- | ---: | ---: |
| Cornell | 870.52 ms | 1.149 |
| Prism | 903.37 ms | 1.107 |
| Pavilion day | 2537.72 ms | 0.394 |
| Pavilion night | 2535.00 ms | 0.394 |

All 160 submitted frames passed accounting and all eight final raw/reconstructed
FLOAT captures were finite/native-resolution. Startup and export are excluded.
These are the retained pre-Enhanced baseline, not new Enhanced measurements or
converged reference images. PR #78 passed all 279 local Debug tests and all eight
hosted checks before merging. Slower execution experiments remain archived and
excluded from main.

## Geometric cancellation recovery

The normalized-cosine calculation now resolves an uncertain rounded zero with
a cold, non-inlined expansion of the three represented signed-scaled products.
Each product retains its explicit FMA residual; General TwoSum preserves up to
six nonoverlapping components without discarding distant exponents. This adapts
[Shewchuk's expansion arithmetic](https://www.cs.cmu.edu/~quake/robust.html) to
the existing scaled representation. The expansion certifies zero and sign for
the represented inputs. Its largest component has relative error less than
`1/(2^23-1)`; correct rounding and recovery of earlier geometric rounding are
not claimed. Nonzero fast-path results remain unchanged. A certified zero dot
retains outside-support semantics, while a certified zero cross component is
omitted from the norm. No tolerance, clamping or sample rejection is introduced.

This repairs a reproduced Pavilion-night failure at sample 38: two rounded
products cancelled, although their exact difference was `-4999589/2^49`.
The recovered value is exactly representable in the scaled format. Release
validation passes 185 GPU geometry/family/query cases with Metal API validation,
including these captured operands, true-zero cancellation, residual-only sums,
widely separated exponents, permutations and signs. The unchanged native
1920×1080 protocol (seed 61002, eight warmup plus 32 measured submissions,
Enhanced paired reuse, zero mutations, suffix caching and signed temporal
reconstruction) now completes all 40 frames. Both final sample-39 FLOAT images
are finite and their metadata/accounting pass independent audit. These checks
establish recovery of that failed sequence, not a quality or speed improvement.
The Wavefront/Area/canonical versions remain 22/16/4; source fingerprint
`4e2db06a` identifies the measured numerical repair. Local evidence is retained
in `enhanced-followup-validation/cancellation-release-001` and
`enhanced-night-cancellation-repair-001` under the hero-performance validation
artifact directory.

## Owned terminal-light stage

The owned terminal-light stage introduced Enhanced canonical law version 2;
the scaled-weight stage introduced version 3, followed by the version-4
preparation stage below. Its disjoint terminal NEE charts own a LightID and either triangle area coordinates, a positional/distant counting mass,
or an environment direction in solid angle. Unsupported emitter shapes retain
generic replay. An imported generic atom that belongs to an owned chart at its
destination has zero mapped support; it does not silently change charts. The
raw physical contribution reevaluates the fixed endpoint's emission, visibility,
BSDF and original four-wavelength technique MIS. It never divides by a sampled
light PDF and then multiplies that division back out.

For nonzero canonical XYZ, the target is `L1(XYZ)/4 + h_eval*epsilon(scale)`;
exact represented zero retains zero target. Fresh local scores are
`target/(h_gen*a)`, where `h_gen` is the actual generated marginal endpoint
density and `a` is discovery probability. The queried `h_eval` may differ in
FLOAT from `h_gen`; replacing one with the other would change the estimator.
This is a new numerical reference, not a claim of bit identity to legacy NEE.
Scaled density arithmetic distinguishes a genuine zero-support cell from
positive products that would underflow an ordinary FLOAT multiplication.

Each selected record travels with its XYZ, target, normalizer and queried
density. MCMC holds the endpoint and contribution-slot labels fixed, reevaluates
spectral/residual coordinates, and retains the entire old tuple on rejection.
One 48-byte record per pixel in each of four generation-owned banks avoids
per-evaluation record copies. Session slots share one arena under the exclusive
Area history lease; independent sessions never share writable arenas. This adds
192 record bytes plus a 32-byte evaluation receipt per pixel. Published frame
captures do not borrow mutable reservoir records.

Enhanced confidence is separate from the reservoir normalizer and age. Fresh
confidence is one, temporal input is capped at 20 before both MIS evaluations,
and spatial output sums structurally present source confidences. Null selected
samples retain their proposal domains. Out-of-bounds self aliases do not invent
neighbors; duplicate valid stencil slots count per occurrence. The original fixed-cap implementation has no extra division by confidence.

Release GPU checks before memoization restoration passed the six physical
fixtures and the independent multibounce mean check, plus three independent
32-seed point/triangle mean contrasts for uniform, power-alias and BVH proposals.
Visibility transitions exercise positive-to-occluded-to-positive history.
The initial pure-Metal probe passed 130 record, geometry, physical-product,
selection-density and environment-density cases, including positive scaled
underflow and explicit invalid-data failures. Injected Area protocol tests check
nonunit densities, capped confidence, null domains, accepted/rejected mutations,
immutable banks and reciprocal-stencil boundaries against independent arithmetic.
After restoring memoization, the physical/NEE controls also pass cache-off/on
comparisons, including shared-AS sessions with nonunit density, null histories,
and camera changes that require replay. The pure-Metal probe now passes 132
cases, adding rejection of negative-subnormal triangle coordinates. The shared
path/confidence/footprint/suffix/reprojection arithmetic, context accounting, CLI
and injected queue protocol checks pass in Release. These are focused stage
checks; final Debug publication checks are separate.
These checks do not qualify four-scene quality, eliminate physical glints, or
establish a speedup. At this owned-terminal stage, adaptive surface reconnection,
suffix caching, shift-pair compaction, dual motion and duplication diagnostics
were still pending. The helper checks did not establish their runtime coverage;
later sections describe the implemented reconnection, suffix, null-admission and
camera-only motion stages.

## Scaled-weight stage

Canonical law 3 keeps Enhanced reservoir score sums `S`, normalizers `Z`, targets
`T`, and spatial cross targets as normalized FLOAT fractions with separate signed
exponents. Serialized positive values have exponent bounds ±4096; exact zero uses
the pair `(+0, 0)`. Arithmetic intermediates may use wider exponent ranges before
storage validation. Exceeding a declared storage or decision bound fails visibly.
Legacy summed/contribution arithmetic retains its existing FLOAT representation.
This extends the numerical reference; it does not establish exact real arithmetic
or unbiasedness of all FLOAT operations.

The native Pavilion-day failure at `6a5091e` was not an invalid fresh path. Its
fresh null was legitimate, while reused history retained `Z ≈ 1.26589e-36` and a
spatial quarter-weight of `1.450105848e-41`. The ordinary FLOAT reservoir could not
carry that positive weight through its normal-only arithmetic. Scaled storage
preserves these positive values rather than discarding the candidate or treating
the history as null. Only final XYZ readout rounds into FLOAT using round-to-nearest,
ties-to-even, including underflow; a rounded-zero output component does not erase
the selected history record or its positive scaled weight.

Enhanced resampling entropy law 1 separates fresh local selection, Area selection
and mutation acceptance by role and counter address. It retains pixel, full
submission serial, seed and site identity and supports up to 257 random words per
bounded scaled decision. It does not extend the legacy entropy strides or change
the separate discovery law, which remains version 1. Wavefront/Area contracts are
18/12; the Enhanced sidecar is 128 bytes, Area history/temporal/accumulator carriers
are 88/92/104 bytes, and each Enhanced cross-target cell is 8 bytes. The owned-light
record and evaluation receipt remain 48/32 bytes. Cached/imported/null evaluation
receipts retain a zero fresh-normalizer exponent; prepare clears it before reuse.

The scaled-weight stage passed the three focused CPU path, scaled-reservoir and
entropy checks, CLI metadata checks, production GPU queue protocol and selected-prefix
controls, and legacy contribution, Enhanced physical/mean and Enhanced NEE tests.
The queue probe includes the exact Pavilion range case and a smaller positive
history whose final XYZ rounds to zero. The first scene-test run stopped at a
stale version-2 assertion; after correcting the asserted version to 3, both
Enhanced suites passed. At `3335bba`, the native rerun completes all 160 submitted frames and eight finite
1920×1080 FLOAT captures. The four-scene timing screen below is complete; quality
and the complete Enhanced algorithm remain unqualified. At that stage, chart
and geometry helpers were preparation for the runtime shifts described below.

For context, the superseded `6a5091e` native screen measured Cornell at
724.195 ms (1.38084 FPS) and Prism at 1073.467 ms (0.931561 FPS). Pavilion day failed
on submitted frame 18, and no Pavilion-night run was obtained, so neither has an
FPS result from that screen. These are bounded pre-repair observations, not final
timings or measurements of the scaled-weight revision.

The repaired `3335bba` runtime uses native 1920×1080, one fresh hero-visible path,
Enhanced owned-light prefix reuse, one spectral-refresh mutation and signed
temporal denoising. Eight warmup and 32 measured static frames per scene, seed
61002, give the following complete-pipeline CPU wall times (startup/export
excluded):

| Scene | Mean frame | FPS |
| --- | ---: | ---: |
| Cornell | 727.31 ms | 1.375 |
| Prism | 1077.60 ms | 0.928 |
| Pavilion day | 3128.71 ms | 0.320 |
| Pavilion night | 2942.07 ms | 0.340 |

This is the verified intermediate baseline before adaptive interior reconnection
and suffix reuse. It fixes the failed Pavilion range sequence; it establishes no
speedup over the merged baseline, and both Pavilion scenes are slower. One static
run per scene is not sustained/motion or converged-quality qualification.

## Hybrid preparation stage

The canonical law 4 preparation captured primary geometry and used a common
geometric BSDF `wo` (toward the preceding physical vertex) for admitted secondary
diffuse/rough-conductor roots in `direct=0/1` events, including events rejected by
footprint eligibility. A neutral interior chart
(tag 5) has a structural codec, Sink projection, persistent state and validators;
terminal-surface tag 6 is rejected. The subsequent runtime stage below activates
these charts. Suffix reuse, dual motion and shift-pair compaction were absent from
this preparation stage; their runtime stages are described below.

Prepared BSDF payloads can retain the union of needed lanes while physical
history keeps its original active mask. The predecessor certificate carries a
separate query mask, applied only to a query copy; zero-beta lanes retain needed
likelihood/PDF information. The predecessor snapshot includes the normal offset
used to reconstruct its query origin. The Enhanced Sink state is 284 bytes,
including a 156-byte hybrid state, 36-byte predecessor snapshot and 20-byte
certificate. Wavefront/Area contracts are 19/13, canonical/record versions are
4/2, and discovery remains version 1. This preparation changes the numerical
reference; the `3335bba` table above measures the earlier range-repair baseline.
No v4 native image or timing result is claimed.

Historical preparation checks passed for CLI controls, footprint and chart
arithmetic, prefix/Sink controls and geometry. The queue probe exposed a premature
record read; lazy validation corrected that failure. Those checks did not cover
forced connections.

## Forced interior reconnection

A successful continuation can now select the first eligible supported interior
pivot. Its chart owns triangle UV and a represented world direction. Native
closest-hit traversal certifies the incoming connection's primitive identity;
it cannot overwrite the owned UV. Imports force the predecessor-to-pivot direction
and the pivot's stored outgoing direction, making the replaced direction draws
inert. Wrong identity, blocked connections, unsupported fixed pivots, or a changed
first-eligible label have zero support in that chart; they do not substitute a
generic path. Invalid arithmetic remains an error.

For an admitted secondary surface, the incoming scattering direction comes from
the represented predecessor and current positions. A native hit can round to the
same stored position as its predecessor. In that exact zero-edge case, retain
the traced ray's validated incoming direction; the zero-length connection remains
ineligible for an owned chart. This does not clamp geometry, drop a contribution,
or replace a nonzero edge whose arithmetic failed.

The rolling predecessor certificate checks whether the newly forced direction
would have made the preceding vertex eligible. Incoming and outgoing hero
updates are recomputed from the saved predecessor beta and density, without
undoing a rounded old update. Evaluation density uses queried PDFs and geometry;
fresh generation density separately uses actual sampling PDFs. Current arrival
emission and direct-light atoms retain their original incoming state; the new
chart is installed only for later atoms. The physical model and finite authored
depth remain unchanged; this extends the canonical-v4 estimator, not just its
execution schedule.

The new 17×17 frame test uses 32 predeclared seeds in a closed diffuse cavity.
Paired raw means and an independent analytic radiance check pass their declared
five-standard-error bounds. It observes 1,060 positive fresh interior records and
20 positive published nonself donor pixels; these are witnesses, not independent
sample counts. Full/prefix replay, queue capacities, exact temporal-cache controls,
and failed-submission recovery also pass. Eleven additional rolling-certificate
controls bring the geometry probe to 92 cases. The native diffuse and rough-.5
conductor probe passes in 63.24 seconds: 16 and seven supported source charts,
respectively, with positive E/N witnesses, full/prefix and replaced-draw controls,
and independent long-double update, geometry and spectral-projection checks.
Discovery is fixed at one in that arithmetic fixture; it is not a mean oracle.
The subsequent native probe also passes blocked-connection and changed-first-label
controls. Full-scene qualification remains pending; no native-v4 scene timing or
Pavilion highlight improvement is established by these fixtures.

The first native-1080p cache qualification at `f539c19` stopped on Cornell's first
frame with both cache settings. Tagged diagnostics identified the coincident-position
case above. After the repair, both first-frame controls complete, and five focused
Release suites pass: legacy contribution, Enhanced physical/mean, suffix execution,
forced hybrid and 111 pure geometry cases. Nineteen added cases check exact incoming
ray bits, signed zeros, invalid fallback directions and nonzero range failures.
This restores the failing case; the four-scene cache/timing study still needs to finish.

## Bounded sequential suffix cache

Enhanced selected-prefix replay now accepts `--area-suffix-cache on|off` (on by
request default). Full replay retains its oracle behavior and leaves the cache
inactive. A captured entry stores at most two ordinary hero updates and the
original terminal E/N factors and represented technique PDFs. Readout applies the
same updates and FLOAT multiplication order as canonical forced evaluation; it
does not replace repeated throughput materialization with a product of ratios.
The immediate emission after a pivot recomputes its represented BSDF PDF from
the live pivot and new incoming direction, retaining the pre-pivot MIS density.

This is a bounded spectral execution adaptation, not the paper's arbitrary-length
RGB suffix factorization. Long, unsupported, uncovered or changed-context entries
replay the original forced evaluator. Misses leave that walk untouched; invalid
complete entries fail visibly. Fresh selection and changed mutation tuples
capture their own entries. Reuse copies each selected record and its matching
tape together, without writing an imported source or restamping an old context.
A checked 64-bit context identity rejects exhaustion instead of wrapping.

Each scalar entry is 160 bytes. Four banks at 1920×1080 require 1,327,104,000 bytes
(about 1.24 GiB), in addition to existing transport/history storage. Allocation
and transient budgets are checked. Reports expose actual enablement, hits for
zero/one/two operations and total misses; these counts are not ray counts or a
per-reason miss histogram. This stage used wavefront 20 and Area 14, with a
40-byte owned evaluation receipt and 292-byte live Enhanced sidecar.

The focused Release controls pass 192 independent CPU tape checks and 17 pure
GPU cases, including ten deferred immediate-PDF receipts. Twenty-four shared-AS
17×17 diffuse frames and four rough-conductor frames retain exact raw XYZ,
selected history, temporal tuples and mutation decisions across camera changes.
Diffuse hit counts are 5,730/4,519/4,046 for zero/one/two operations, with 7,688
misses; conductor counts are 251/100/42, with 105 misses. Allocator exhaustion,
accepted asynchronous failure/recovery, immutable source copies and inactive
full-replay controls also pass. These are bounded correctness witnesses, not
native-resolution quality or performance qualification.

A standalone GPU probe exposed optimization-sensitive loss of a scaled
likelihood lane: an inlined ready readout disagreed with the independent oracle
and with deferred completion. The same probe passes at `-O0`; at the normal
`-O1`, retaining a call boundary around readout also passes all 17 cases. The full
lookup has its own boundary, qualified by the frame controls. The validated
compiler is Apple Metal 32023.883. No arithmetic tolerance or failure gate was
relaxed; these boundaries are a measured code-generation workaround, not a claim
that a compiler defect's root cause has been established. Failed diagnostic and
optimization variants remain in the local validation archive.

## Native suffix screen and selected-null admission

At `fa09f29`, all four native 1920×1080 scenes retain exact raw XYZ, selected
history, temporal payloads and referenced owned records through 40 shared-AS
frames with suffix caching off/on. This correctness comparison disables temporal
memoization in both arms. A separate static timing screen enables exact temporal
memoization in both arms and includes signed temporal reconstruction with the
conservative variance policy: eight warm-up frames, 32 measured frames, seed
61002, one fresh visible-CDF hero path and one spectral-refresh mutation.

| Scene | Cache off, ms/frame | Cache on, ms/frame | Cache on, FPS |
| --- | ---: | ---: | ---: |
| Cornell | 1210.41 | 1130.58 | 0.8845 |
| Prism | 1690.36 | 1705.23 | 0.5864 |
| Pavilion day | 3465.01 | 3476.91 | 0.2876 |
| Pavilion night | 3502.30 | 3508.26 | 0.2850 |

FPS is measured-frame count divided by total complete-pipeline wall time.
Cornell gains about 7%; the other scenes show no established benefit in this
single screen. All final raw/reconstructed captures are finite FLOAT RGB.
These are single-frame estimates with history, not accumulated reference images.
This screen establishes neither sustained throughput nor equal-quality speedup.
It measures the current hybrid implementation; earlier scaled-weight timings
predate interior reconnection and are not an execution-only cache baseline.
The 50/100 ms targets remain unmet.

`--area-pair-compaction on` adds a separate, default-off execution experiment
for Enhanced Area. Each leaf validates every logical request before queue
admission. Structurally selected-null requests publish their original zero result
without loading transport state; real requests append actual pixel IDs and enter
the existing wavefront. All fresh owners run. A damaged request fails rather than
becoming a cheap null. Sources and source suffixes remain immutable; mutation
scratch is cleared only for its exclusive result owner.

The work list costs four bytes per pixel per frame slot. Completion claims and
refill positions retain pixel identity through nondeterministic admission order.
The reported compacted-null count is a subset of analytic-null evaluations,
never additional work in the logical accounting. The admission stage used
wavefront contract 21 for the work-list binding and admission count, with Area 14.
This implements selected null admission, not arbitrary prediction of positive
suffix-cache hits. It keeps
the four existing spatial leaves and their proposal coefficients.

Seven focused Release tests pass (656.07 s), with Metal API validation on the
GPU suites: CLI/schema checks, actual queue admission/refill guards, contribution
and Enhanced transport, exact shared-AS compaction controls, and suffix controls.
The compaction controls cover full/prefix replay, zero/one mutation, black/emitting
fixtures, capacities 1/37/49 and static/changed camera contexts. Native performance
adoption is separate; the option remains off by default.

At `62dca50`, all four native-1080p shared-AS controls preserve exact raw XYZ,
history, temporal tuples, referenced records and mutation decisions for eight
frames from cold history. Each scene exercises selected-null admission. Separate
static timing arms use seed 61002, eight warmup and 32 measured frames, one fresh
hero-visible path, Enhanced prefix reuse, one spectral-refresh mutation and signed
temporal conservative reconstruction. Suffix caching and exact temporal
memoization are enabled in both arms:

| Scene | Compaction off | Compaction on | On FPS |
| --- | ---: | ---: | ---: |
| Cornell | 1184.44 ms | 1138.57 ms | 0.878 |
| Prism | 1704.68 ms | 1743.25 ms | 0.574 |
| Pavilion day | 3465.19 ms | 3608.62 ms | 0.277 |
| Pavilion night | 3551.44 ms | 3668.69 ms | 0.273 |

All 16 final raw/reconstructed FLOAT captures are finite at 1920×1080. This
single static screen finds a 4.0% Cornell FPS improvement and 2.2–4.0% regressions
elsewhere; it supports retaining the option off by default. Exactness covers the
shared-AS eight-frame controls, not independent-process timing captures. These
are complete frame latencies excluding preparation/export, not sustained, p95,
convergence or matched-quality results.

## Camera-only temporal reprojection

`--area-temporal-reprojection dual` enables experimental camera-only temporal
source selection for Enhanced wavefront Area; `off` remains the default. Before
Area resampling, a deterministic center-film, center-lens query from the resolved
physical camera captures geometry and freezes one map `donor[d]=s`, independently
of physical replay coordinates and selected atoms. Ordinary correspondence uses the old visible surface; disocclusion uses
the stored old foreground point `O` and current point `X`:
`project_old(O) + project_current(X) - project_current(O)`. Unusable correspondence
retains the explicit same-pixel source domain `s=d`; it does not discard history
or clamp an offscreen donor. The construction follows
[Zeng et al., §4.3](https://sites.cs.ucsb.edu/~lingqi/publications/paper_trmv.pdf).

Forward evaluation uses the current context at `d` with history from `s`;
reverse evaluation uses the previous context at that same `s` with fresh replay
from `d`. Old targets, normalizers and confidence also come from `s`. Multiple
owners may share a donor. Camera sampling and sensor validation use `s` for the
mapped reverse evaluation, while receipts, mutation scratch, completion and
output remain owned by `d`. Physical transport and MIS formulas are unchanged;
the selected temporal source domains can differ. This runtime uses wavefront 22
and Area 15.

An unchanged complete evaluator context reuses committed geometry and skips the
donor pass, retaining same-pixel temporal memoization. Geometry and reservoir
history publish together only after successful completion and matching serial
validation. Their fronts are independent; a failed attempt cannot expose dirty
geometry as committed history, and cuts invalidate both eligibility states.

The historical 129 CPU geometry/projection controls cover independent parallax,
rebasing and discrete foreground anchors. The integrated Release stage passes 11
focused suites in 666.94 seconds, including actual GPU donor generation, mapped
queue/Area protocol, moving-frame publication/recovery, legacy contribution,
Enhanced physical/mean, compaction and suffix controls. Metal API validation is
enabled on GPU suites. A separate successful 7×7 eight-frame CLI sequence checks
reported serials/generations, translation, static memoization and camera cuts.
The moving depth-zero mean control is a bias smoke test, not general scene-quality
qualification.

At `b720693`, a native-1080p eight-frame hold/translate/hold/cut sequence completes
on Cornell, Prism and Pavilion day/night with one mutation and signed temporal
reconstruction. All 32 frames have the expected geometry, donor, cache and
publication accounting; every scene exercises positive dual-donor selection.
All eight final raw/reconstructed FLOAT captures are finite. This is a bounded
moving-scene integration check using reciprocal spatial offsets, not a performance
comparison or motion-quality qualification.

This stage supports static resident geometry with camera motion only, without
object or deformation motion. The early geometry query duplicates the later
reconstruction guide query; its performance benefit and moving-camera raw quality
remain unqualified.

## Paired Gaussian spatial domains

`--area-stencil paired-gaussian16` adds the paper's §3 pairing construction.
Four immutable square tiles use widths 254/230/210/194, sigma 16, and 128 alternating
2×2 permutation passes from Eq. 3. Equal initial labels occur exactly twice.
Independent uniform permutation draws and deterministic endpoint discovery produce
signed reciprocal deltas; independently seeded flips, transposition and offsets
vary the pairs per accepted frame. Four tiles preserve our existing four-neighbor
work/weight budget; the paper's evaluated configuration uses three. The fourth
width and deterministic CPU endpoint discovery are declared adaptations.

The infinite-plane pair map is an involution. A pair crossing the image boundary
becomes a self singleton without wrapping/clamping the image. Reverse targets use
the same atlas slot at the paired endpoint, rather than a neighboring legacy
plus/minus slot. Duplicate slots keep their original coefficients. Integer
coordinate reduction precedes sign transforms, avoiding signed 64-bit remainder
operations in GPU lookups while retaining wide final image bounds. Malformed
entries fail before boundary fallback.

The immutable 199,152-entry asset occupies 398,304 bytes and is shared by sessions
using one resident scene. Each frame slot retains its own 112-byte pose/config;
legacy modes use typed sentinels without generating the asset. Area contract 16
reports both element and config sizes; wavefront remains 22. JSON/EXR metadata
retain the actual asset hash, sizes, seed, sigma, iteration count and per-frame
poses. The version-one byte hash is
`6d877656f1338b07f72e2a917fb5f491cfb1952a5a476fba981b027b3e2605db`.

The CPU generator matches an independent complete-byte construction and frozen
small/production goldens. Predeclared sigma/radial/covariance screens pass; these
moments do not establish a complete Gaussian fit. All 20,884 CPU/GPU lookup
controls pass, including finite-image reciprocity, malformed entries, full-range
pixel coordinates and explicit premature-signed-cast negative controls. Actual
Area pairwise-MIS and 53 qualification-identity controls pass. Integrated atlas
rendering passes in 55.12 seconds, including exact cache/compaction/scheduling
controls and a 32-seed emission-mean smoke comparison. This simple mean fixture
does not qualify complex scene quality. Successful CLI checks cover an eight-frame
paired-atlas/dual-motion/compaction sequence and a 48-frame, three-arm qualification
run with 30 finite 16×8 FLOAT captures. The latter verifies actual configuration,
accounting and output integration, not image quality or throughput.

The qualification driver now exposes Enhanced/prefix, paired stencil, cache,
compaction and motion controls, mutation-arm selection, large-scene budgets and
temporal variance policy. It validates actual frame identity before labeling or
reconstructing results, keeping independent hero references separate from Area
arms. Complete scene-quality and native throughput qualification remain pending.

A subsequent native-1080p paired-atlas zero-mutation screen on `9d34076` completes
Cornell, Prism and Pavilion day, but Pavilion night fails wavefront validation
at sample 38. The failure is retained for diagnosis; the four-scene timing study
is incomplete. Optional duplication diagnostics were absent from that runtime
and do not establish a repair.

## Enhanced fresh-only diagnostic

`--area-reuse fresh` extracts the actual Enhanced fresh local-selected `(U,j)`
with its discovery normalizer `Z` and physical target `T`, producing `Z*F/T`.
This is one selected-contribution estimator, not the independent PT path sum.
It requires Enhanced wavefront Area, explicit zero mutations and spatial mode
`all`. It rejects reprojection, pair compaction and duplication diagnostics.
The default `--area-reuse full` retains the existing pipeline.

Each pixel runs one complete fresh discovery evaluation. A dedicated finalizer
validates its confidence-one accumulator and writes the weighted raw estimate;
zero/null reservoirs produce zero without materializing `0/0`. There is no
extra categorical draw, temporal proposal, mutation or spatial replay. Completion
reports `physical=logical=fresh=N`, zero cached/analytic-null evaluations, and zero
temporal/spatial proposals. Finite-depth and scene-model requirements are unchanged.
Both selected and conditional-mean output modes use this same one-atom readout.

Accepted attempts consume their serial, including failed attempts. Successful
fresh completion releases the pending frame without flipping the history front,
incrementing its generation, assigning a committed reservoir serial or making
history valid. Repeated samples and camera/epoch changes retain these rules.
Per-frame accumulator/XYZ storage follows the ordinary retained-Frame lifetime;
ordinary reusable-history snapshots remain unavailable for this diagnostic.
Physical temporal/suffix caches are effectively off and reported separately from
their requested settings. The diagnostic currently retains ordinary Area storage;
it is a work/noise-floor isolation, not a minimal-memory renderer.

The frame benchmark supports `--reconstruction none`, `temporal` and `metalfx`.
Reconstruction retains its separate output and history; it cannot publish physical
Area history. Qualification accepts this mode with `--mutation-arms 0` and keeps
its independent reference/arm distinct from Enhanced fresh extraction. A shared-AS
oracle compares raw extraction with `Z/T` readout of the cold full pipeline's
captured fresh temporal reservoir. CPU identity and GPU controls distinguish
numerical equivalence, null/range behavior, epoch changes, retention and failed
attempt recovery from scene-quality or native-throughput qualification.

## Experimental stochastic spatial terms

`--area-spatial-mode stochastic-pairs` opts into our stochastic evaluation of
four existing pairwise-MIS terms. It requires Enhanced wavefront Area and
`paired-gaussian16`; `all` remains the default. Both the benchmark and
qualification CLI retain requested/effective mode and actual per-frame atlas IDs.
This changes the estimator's variance and selected history, not the physical
transport model. It is not a paper performance result or a quality qualification.

Condition on frozen temporal reservoirs and all four frame atlas poses. Write
`D_k` for the donor score and `C_k` for the center score, each already including
its existing coefficient `1/4`. Before spatial work, draw independent uniform
`I,J` in `{1,2,3,4}` from separate Philox addresses keyed by accepted frame serial
and seed. They are shared by all pixels, preserving atlas `J`'s actual involution
for the reverse-target lookup. Reservoir resampling combines scores `4*D_I` and
`4*C_J`. Its selected normalizer is their sum; there is no extra division by two,
four, or confidence. Conditional-mean readout uses the same two weighted terms.
Averaging over the sixteen index pairs restores the original four-term sums.
This conditional identity preserves the ordinary input-UCW argument; independent
raw scene quality and correlation behavior remain separate validation work.

Evaluate the immutable donor-target map for atlas `I`, and independently the map
for atlas `J` needed by the center's reverse lookup. When `I==J`, one completed
map supplies both roles. A canonical-only pass never adds another donor or enters
selected history. Each frame therefore evaluates one or two spatial maps, while
retaining every admitted domain's structural confidence across all four atlases,
including duplicate slots and zero/null draws. Border singletons retain their
existing canonical contribution and add no donor confidence. No map is skipped
based on its observed radiance or sampled target. Motion, cuts and suffix-cache
support retain their existing physical replay and publication rules.

`--area-spatial-mode stochastic-shared` is a separate opt-in policy. Draw one
uniform atlas `K` and use `I=J=K`: scores are `4*D_K` and `4*C_K`, evaluated from
one completed target map. The average over the four diagonal choices equals the
same original sum. Independence between the two term sums is unnecessary for
this linear expectation; sharing can change variance and spatial correlation.
Its forward draw is exactly the independent policy's forward RNG site. The
independent `stochastic-pairs` mode remains available, and `all` remains default.
No data-dependent index choice or retry is introduced.

The independent pilot used Area contract 17. Contract 18 retains the 116-byte
config, declares the three-valued policy and two 1-based IDs, and rejects
non-diagonal IDs in shared mode. Both CLI/API policy identities distinguish the
two stochastic estimators. CPU checks enumerate 16 independent pairs and four
diagonal pairs, then recursively propagate the selected normalizer for three
generations with signed, null and border cases, comparing selected and conditional
means. Production-kernel tests contain explicit policy controls; native shared-mode
validation remains separate from the frozen independent pilot.

Disabled configs use zero IDs. Completion accounting reports actual spatial map
work and validates the serial/seed-derived choices; this is not four completed
spatial evaluations relabeled as one. Area contract 20 binds one scaled cross-target map per slot for either stochastic
policy. The donor-only pass writes its score directly into the accumulator. Only
the canonical atlas writes the reverse target needed by the final pass. This saves
99,532,800 bytes across two native 1080p slots, compared with four maps per slot. CPU exhaustive checks and production-kernel controls cover
all sixteen choices, signed means, nulls, borders, duplicate neighbors, selected
weights and immutable records. Their results do not establish native throughput.

## Optional duplication diagnostics

`--area-duplication off|initial-seed|initial-atom|current` is diagnostic-only and
defaults to `off`. It requires wavefront Enhanced Area. Disabled sessions retain
the original Area entry points and allocate no diagnostic buffers or dispatches.
Under the fixed policy these maps only collect measurements. The experimental
duplication policy below consumes the initial-seed map.

Fresh winners receive an exact 16-byte provenance label: original pixel, 64-bit
sample label and initial contribution slot `j`. `initial-seed` compares the pixel
and sample words; `initial-atom` also compares the original `j`. Repeated labels
can share this family despite different serial-keyed discovery/selection choices;
it is not a complete fresh-event or transport identity. Ancestry follows the
actual winning owned record through temporal and spatial reuse and remains
unchanged across accepted and rejected MCMC moves. This genealogy can overstate
correlation after mutation. `current` instead compares the seven current replay
words and `j` as an exact 32-byte key, with the common replay version validated
separately. Equality or inequality in any mode does not prove independence.

Each valid center counts matching valid neighbors in a finite-image 17×17 square,
excluding itself. Out-of-bounds and missing neighbors contribute zero; the
denominator stays 288 at edges. Null centers store the explicit `0xffff` missing
count, not zero duplication, while retaining their physical proposal domains.
Unknown positive ancestry is a protocol failure. A 291-bin histogram distinguishes
counts 0–288, null centers and unknown positive centers.

The separate version-one diagnostic ABI uses a 32-byte config and 1,180-byte
counters; normal wavefront 22/Area 16 carriers and bindings remain unchanged.
Enabled global storage is `52*N` bytes for three 16-byte label arrays and two
2-byte count maps, plus small per-slot config/counter/argument buffers.
Reservoirs, labels, maps and copied statistics publish together after physical
and diagnostic accounting and serial checks. Failed attempts cannot expose dirty
destination maps; cuts invalidate eligibility. Private latest-publication readback
returns owned CPU copies under no-pending/serial/generation guards, rather than
lending mutable GPU storage to retained frames.

Release validation passes the CPU label oracle, GPU tiled count/guard controls,
CLI validation, and real-frame off/on exactness and publication-lifetime tests
with Metal API validation. The frame controls compare exact raw pixels, physical
reservoir tuples and selected records for all three modes, including repeated
sample labels, 64-bit sample indices and forced failure/recovery. Separate actual
CLI runs pass eight-frame paired-atlas/dual-motion/compaction sequences for each
mode. Native-resolution overhead and full-scene diagnostic distributions remain
unmeasured. No decorrelation, highlight repair or performance benefit is established.

## Retained measurements and limitations

The `3ed5432` runtime's native study used three seeds, static and phased-motion
scenarios, eight warmup and 32 measured frames per run: 48 runs and 1,536 measured
frames. Median paired speedups were 3.65/3.36× for Cornell static/motion,
3.90/3.59× for Prism, 2.33/2.13× for Pavilion day and 2.26/2.08× for Pavilion night.
These timings are not quality-qualified: 20 of 48 final raw/reconstructed FLOAT
comparisons differ, all in Pavilion. All captures are finite; Cornell and Prism
comparisons are exact. This bounded study is not the default 96-frame protocol.
It includes a recorded pause between conditions and ordinary desktop activity.

Pavilion day/static/seed 61002 controls retain the failure. Restoring the earlier
52-byte native metadata layout or disabling temporal caching does not restore
baseline identity. An unchanged baseline repeat also differs, with a smaller
observed maximum difference. These facts do not establish a sole cause. A follow-up static native-1080p
Pavilion-day diagnostic (seed 61002, 40 frames including eight warmup submissions)
built one shared native acceleration structure and used independent session slots
and history banks. Cache-on and cache-off raw XYZ matched bitwise in every frame;
mutation/accounting checks also matched. This isolates memoization on that AS. It
does not compare reconstructed output or qualify independent AS builds, motion,
other seeds, or Pavilion night.

Separate native-1080p bare-PT measurements on that runtime, with eight warmup and
32 measured frames, give Cornell 129.44 ms/frame (7.73 FPS) and Prism
141.18 ms/frame (7.08 FPS). A short denoiser screen adds about 27–38 ms for signed
temporal/spatial reconstruction and 10–12 ms for native MetalFX. Neither those
figures nor the combined throughput meet the complete-pipeline target.

Two execution experiments were rejected for default adoption. Splitting material
preparation from the transport event preserved all paired raw FLOAT controls,
but made the native Cornell/Prism screen about 33–37% slower. Xcode still reported
192 registers in both kernels, with 3.25 KiB and 4.16 KiB of spills. The additional
boundary did not remove the expensive shader storage. Caching normalized
geometric normals preserved paired raw/reconstructed FLOAT pixels, but a native
two-scene screen showed no meaningful complete-pipeline gain and costs about
33 MB at 1080p. Both controls are archived and excluded from this implementation. These short experiments
used two warmup and three measured frames and are not sustained qualification.

The acceleration stack (#74–77) was merged in `a6d7503`. All required hosted
checks passed; the macOS job needed one unchanged retry after pipeline creation
returned nil in a standalone arithmetic probe. The original failure and the
successful retry are retained separately. The merged stack does not close the
long-history Pavilion qualification gap above.

Raw reports, frozen executables/libraries, failed controls and provenance remain
outside Git in `.cache/validation/hero-performance-implementation-001/`.

Primary foundations: [GRIS](https://graphics.cs.utah.edu/research/projects/gris/),
[corrected MCMC mutations](https://arxiv.org/abs/2211.00166),
[Area ReSTIR](https://research.nvidia.com/publication/2024-07_area-restir-resampling-real-time-defocus-and-antialiasing),
and [wavefront path tracing](https://research.nvidia.com/publication/2013-07_megakernels-considered-harmful-wavefront-path-tracing-gpus).

## Temporal confidence policies

`--area-history-cap N` sets the previous frame's maximum confidence for
Enhanced replay. The supported range is 1 through 20. The default is 20.
The cap enters both sides of temporal MIS and the published temporal
confidence. Spatial passes accumulate the confidence of all admitted sources
as before. The cap stays constant over the session and is independent of each
sample's radiance, selection, or duplication under the default fixed policy.

The benchmark, qualifier and viewer accept `--area-history-cap-policy duplication`
for full Enhanced Area replay. This opt-in policy requires the initial-seed
ancestry map. For each historical donor, its previous published map stores
the number of other pixels sharing that initial seed in a 17×17 window. The
center is excluded. Out-of-bounds pixels contribute zero. The denominator
is always 288. With user maximum `H` and matching fraction `D`, the cap is
`H + (1 - H) * D^0.1`. The shader applies the lower of this cap and the
donor's structural confidence to both temporal MIS terms. Fractional confidence
survives accumulation and spatial MIS. Reprojection reads the count at the
same previous pixel as its reservoir. Cuts invalidate both histories.

A selected-null donor has no seed to match. Our adaptation assigns it zero
duplication and retains its structural confidence subject to the ordinary
maximum. Invalid ancestry on a positive donor is a frame error. Conditional
weights depend on the selected sample under this policy and can introduce
bias. Lin et al. measured 3.25% average absolute relative bias in their
Kitchen scene. That value applies to their experiment. The published method
and our Area implementation need separate image and cost measurements.
[Lin et al., section 5](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/lin2026restirptenhanced.pdf)
