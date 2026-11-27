# Enhanced ReSTIR development

The target is native 1920×1080, one fresh four-wavelength hero path per pixel,
Area ReSTIR, one corrected MCMC mutation, and denoising: **20 FPS for Cornell and
Prism, and at least 10 FPS for Pavilion day and night**. These are complete-pipeline
budgets of 50 ms and 100 ms per frame. Bare path-tracing throughput is diagnostic,
not the acceptance criterion. Report static and moving-camera results, frame-time
tails, startup, and image quality separately. Preserve authored scene semantics;
resolution reductions, model simplifications, radiance clamping, and biased reuse
are not implicit ways to satisfy these targets.

## Current implementation and paper coverage

The existing Area mode is a full-replay GRIS adaptation. It does not yet implement
all of [ReSTIR PT Enhanced](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/)
or the original paper's hybrid reconnection architecture. Its warm static frame
performs six full path evaluations per pixel after exact temporal memoization:
one fresh evaluation, one mutation, and four spatial evaluations.

| Mechanism | Current coverage |
| --- | --- |
| Contribution-aware resampling MIS | Source/destination target adaptation |
| Confidence recurrence, §2.2 | Enhanced caps temporal input at 20 before MIS and sums structural spatial inputs |
| Reciprocal spatial work sharing, §3 | Reciprocal stencil and shared reverse targets; not the paper's pairing textures |
| Final vector-weight shading, §6.3 | Conditional-mean readout, separate from the selected history sample |
| Contribution-path reservoir, §§2.3 and 6.1 | Opt-in finite-depth `(U,j)` with fresh local selection; full or selected-prefix replay |
| Hybrid reconnection and Jacobian, Eq. 2 | Owned triangle/world-direction charts for admitted diffuse and rough-conductor roots; forced incoming/outgoing evaluation and scaled chart densities |
| Dual-footprint eligibility, Eq. 5 | Runtime first-eligible selection, forward/inverse footprints and rolling predecessor veto |
| Forced NEE endpoint reconnection, §6.2.3 | Enhanced owns triangle-area, positional/distant-mass and environment-direction endpoints; other shapes retain replay |
| Cached suffix evaluation | Bounded two-operation sequential hero tape; exact prefix replay on misses |
| Selective shift-pair compaction, §6.2.2 | Opt-in admission skips structurally selected-null requests; all nonnull requests retain canonical evaluation |
| Initial-only external roulette, §6.2.4 | Opt-in `enhanced`: independent discovery gates and canonical no-outer-roulette imports |
| Reservoir reprojection and dual motion, §6.4 | Missing; denoiser motion vectors do not implement this |
| Duplication maps, §5 | Missing; adaptive confidence introduces bias and must remain a separately labeled option |

Published performance is evidence about that implementation and hardware, not a
forecast for M4 Max or for our spectral/coated-material model.

## Implementation order and contracts

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
neighbors; duplicate valid stencil slots count per occurrence. There is no
extra division by confidence and no duplication-dependent biased cap.

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
establish a speedup. Adaptive surface reconnection, suffix caching, shift-pair
compaction, dual-motion integration and duplication diagnostics remain pending.
The footprint, suffix-product and reprojection helpers are preparatory algebra;
their presence alone is not runtime coverage of those mechanisms.

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

The current stage passes the three focused CPU path, scaled-reservoir and
entropy checks, CLI metadata checks, production GPU queue protocol and selected-prefix
controls, and legacy contribution, Enhanced physical/mean and Enhanced NEE tests.
The queue probe includes the exact Pavilion range case and a smaller positive
history whose final XYZ rounds to zero. The first scene-test run stopped at a
stale version-2 assertion; after correcting the asserted version to 3, both
Enhanced suites passed. At `3335bba`, the native rerun completes all 160 submitted frames and eight finite
1920×1080 FLOAT captures. The four-scene timing screen below is complete; quality
and the complete Enhanced algorithm remain unqualified. The chart
and geometry helpers remain preparation for future runtime shifts.

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

The in-progress canonical law 4 captures primary geometry and uses a common
geometric BSDF `wo` (toward the preceding physical vertex) for admitted secondary
diffuse/rough-conductor roots in `direct=0/1` events, including events rejected by
footprint eligibility. A neutral interior chart
(tag 5) has a structural codec, Sink projection, persistent state and validators;
terminal-surface tag 6 is rejected. The subsequent runtime stage below activates
these charts. Suffix reuse, dual motion and shift-pair compaction remain pending.

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
per-reason miss histogram. Contracts are wavefront 20 and Area 14, with a
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
never additional work in the logical accounting. Wavefront contract 21 adds the
work-list binding and admission count; Area remains 14. This implements selected
null admission, not arbitrary prediction of positive suffix-cache hits. It keeps
the four existing spatial leaves and their proposal coefficients.

Seven focused Release tests pass (656.07 s), with Metal API validation on the
GPU suites: CLI/schema checks, actual queue admission/refill guards, contribution
and Enhanced transport, exact shared-AS compaction controls, and suffix controls.
The compaction controls cover full/prefix replay, zero/one mutation, black/emitting
fixtures, capacities 1/37/49 and static/changed camera contexts. Native performance
adoption is separate; the option remains off by default.

## Camera-motion geometry foundation

Portable camera projection and geometry compatibility helpers now cover a
camera-only dual-motion donor construction from the old foreground point:
`project_old(O) + project_current(X) - project_current(O)`. The 129 CPU controls
use independent double-precision camera/parallax expectations, common-origin
rebasing, discrete foreground anchors and explicit geometry thresholds. Invalid
projection inputs fail; valid offscreen/near-plane cases retain a labeled
same-pixel domain fallback. This follows the geometric construction in
[Zeng et al., §4.3](https://sites.cs.ucsb.edu/~lingqi/publications/paper_trmv.pdf).

These helpers do not yet produce GPU geometry, retain matching history geometry,
or map the reverse transport owner to the selected old pixel. Runtime integration
must consolidate the existing projection-level donor helper with this geometry
layer and validate both domain directions. No dual-motion quality or performance
benefit is claimed by the CPU controls.

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
