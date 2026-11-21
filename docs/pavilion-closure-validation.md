# Pavilion correctness and performance closure

This continues [PR #51](https://github.com/rlefko/rypath/pull/51) after published
revision `3a18a02` (renderer 20). The earlier
[transport study](transport-regression-validation.md) remains historical evidence,
including its failed quality bounds. The changes below do not establish scene
parity or completion of the performance target.

## Coating proposal measure

Renderer 21 corrects two internal MIS comparisons in scalar and packet layered
directional evaluation. Let `wi` be the external endpoint and `w` the internal
propagation direction. The virtual-light proposal samples transmission from
`wi` in the opposite transport mode, producing the internal direction `-w`.
Its density in that internal solid-angle measure is therefore
`PDF(wi, -w, !mode, Transmission)`. The former expression
`PDF(-w, wi, mode, Transmission)` instead describes the external direction
conditional on incidence from inside the coating. These are different random
variables. Near normal incidence their densities differ by the refraction
Jacobian; they cannot be compared directly with an internal base or phase PDF.
Negating the direction has unit solid-angle Jacobian, so the corrected query
does not need another eta-squared factor.

The forward throughput remains `f(-w, wi, mode)`. The repair changes the
base-sampled and phase-sampled complementary MIS weights only. Delta handling,
random draws, joint `Sample_f`, directional PDF proxy and finite layer-depth
policy remain unchanged. The target scattering model is unchanged; this is an
estimator correction. Checkpoints from renderer 20 and earlier are rejected.

The pinned PBRT `b0bdf461` implementation contains the same two forward PDF
queries. Consequently, corrected regression values are explicitly a modified
reference, with stock values retained. A first attempted reference patch changed
only the header included by the wrapper; PBRT's extern-template implementation
still came from its old archive. Its zero-change result was rejected by the
effect check and retained. Recompiling `bxdfs.cpp` and linking that explicit
object before the archive changed 204,560 of 524,288 directional-f results,
while every other returned field stayed bit-identical.

An independent absorption-only diffuse-base composition reference uses the
dielectric interface function, deterministic angular quadrature and a geometric
sum of base round trips; it does not call layered `f`, `Sample_f` or `PDF`.
The finite five-base-order model matches the support of the actual even layer
depth 10. An odd depth-1 support discrepancy exists but does not explain the
depth-10 result. Four fixed angular grids include a half-bin azimuth shift;
the finest grid and its shifted counterpart differ by below 1e-6 in the reported
means. This is observed quadrature agreement,
not a certified integration bound.

A paired zero-reflectance control removes the common direct-interface sampling
noise from the reduced near-normal comparison. It uses 32 groups of 4,096
queries, with the same control shared by all four spectral lanes; these are not
four independent control populations. Base-only directional-f minus joint-walk
estimates are:

| Lane | Former mean difference | Paired SE | Corrected mean difference | Paired SE |
| --- | ---: | ---: | ---: | ---: |
| 0 | 0.00230631 | 0.00021919 | 0.00002077 | 0.00021528 |
| 1 | 0.00142391 | 0.00013108 | -0.00002210 | 0.00013014 |
| 2 | 0.00343393 | 0.00032517 | 0.00004993 | 0.00031870 |
| 3 | 0.00279274 | 0.00026119 | 0.00000089 | 0.00025680 |

All fixed queries and six cross-lane contrasts are retained. This diagnostic
supports the local correction; it is not a new scene-quality acceptance test.
The traced daytime outlier uses a joint-sampled coating continuation, so this
directional-f repair does not remove that demonstrated heavy-tail mechanism.

Regression validation on the renderer-21 working tree includes:

- 64 surface and 128 phase proposal-partition checks against independent
  dielectric formulas, including near-normal and grazing endpoints.
- All 28 scalar and packet schedule fixtures: 16 directional-f values change;
  the other seven fields per fixture remain exact against the stock reference.
- Twelve packet moment routes with 131,072 fixed queries each, including
  diffuse/conductor bases, participating coatings and both transport modes.
  The stock oracle reproduces all 120 original targets. The corrected reference
  changes only the 48 directional-f and weighted-f moments; all 72 joint-weight
  and PDF targets remain exact. Statistical tolerances are unchanged.
- Release scalar and packet Metal tests pass with API validation enabled.

Schedule regeneration is recorded by
`scripts/update_layered_schedule_oracles.py` and
`tests/layered_schedule_corrected_f.json`. The authenticated packet-moment
proposal SHA-256 is
`da53b157642728af8f48f971572309ae189e7633a930a432bb6ed5ee6c4ee7a5`.
Full scene comparisons and actual transport-strategy context checks remain
separate requirements.

A subsequent complete modified-PBRT rebuild includes all 77 library translation
units and the CLI, so both explicit layered instantiations and inline consumers
use the corrected coating proposals. It also retains the reviewed importance
shading-normal and one-sided emission-support corrections. All 87 build commands
succeed, with the archive's ordered object payloads checked and inputs unchanged.
The retained stock helper reproduces all 28×8 fixture bits; the rebuilt corrected
helper matches all 28 directional-f targets and preserves the other 196 fields.
The angular-support test passes 6,216 checks over 3,089 queries, and the two fixed
day/night debug paths pass their finite-contribution/support guards. This is
modified-reference admission, not unmodified PBRT ground truth or a scene-quality
result. The admission receipt is
`ac552c67fa6d46624824fc68aa4ce12b8fe57ed407e9c6cf4ae7a412bc9849c3`.

## Cross-strategy density consistency

A separate GPU probe calls the production BDPT balance heuristic for all three
strategies of eight explicit camera/coating/environment paths. It retains each
strategy's own densities and checks the weights against the independent rational
family `[C*S, C*O, L*O]`. Four identity-frame cases share the same density keys
and sum to one within float rounding. Four rotated-frame cases do not: the
near-normal pair has an exact-rational sum error of `+0.005873120362`, and the
grazing pair has `-0.000463682674`. Individual GPU weights differ from their own
rational values by less than `4.8e-8`, excluding log/exp rounding as the cause.

The sampled continuation retains its original local directions; a newly
connected endpoint reconstructs them from world geometry. A few changed float
bits select unrelated random streams in the direction-hashed coating PDF
estimate. For the near-normal pair the resulting proxy changes from
`0.90001905` to `0.84592426`. The same mismatch occurs with a zero-reflectance
base, which isolates the density calculation from directional-f evaluation.

This is a demonstrated inconsistency in reachable production query paths, not
a measurement of scene bias: the explicit paths are constructed rather than
replayed generated coating walks. A repair must use one represented-path
convention for both fresh MIS queries and stored forward/reverse densities,
while preserving sampled joint throughput, delta events and endpoint measures.
Changing only the fresh query leaves inconsistent denominators. These checks
do not establish that a directional PDF proxy equals the hidden-walk marginal.

An isolated canonical-density prototype also has actual-walker evidence: all
427 observed strategy families satisfy the common rational partition, versus
81 of 427 with the old density convention. The largest GPU weight discrepancy
from the independent rational value is `1.28e-7`. Across 37 contexts and four
captures per context (two arms, each observed and unobserved), material fields and within-arm sample/splat
controls pass; the material checks cover 7,952 coating lanes per arm. This
supports the density repair but is not a scene-quality result.

That prototype's adoption was blocked by a separate constructed-context failure: four
nonzero-base cases lose all four nested reflectances in two stored vertex
copies, changing twelve strategy lighting results. The original binary
reproduces exactly. Enabling GPU validation restores precisely those 32 material
words and 96 lighting words, with no reported fault; all other captured words
remain unchanged. Instrumentation changes execution and cannot establish the
cause or serve as a production repair. The canonical-density prototype remains
outside the renderer-22 integration described below.

A reduced version still reproduces the failure after removing generated
continuation work and the actual MIS-weight invocation. Removing only its four
trailing PDF queries then restores the same 32 material and 96 lighting words;
the positive control and every other retained output and typed field stay
unchanged. The original failing binary is replayed first. This rejects that cut
as a smaller failing reproducer and identifies sensitivity to the query block's
presence. It does not diagnose a compiler defect or justify removing queries
from production. The failed-preservation receipt is
`c83f803c9a220d7928017142b0c4f3877302a1b30927f252cb337402eacfb72a`.

Further reduction isolates the direct reverse-PDF query: the passing and failing
intermediate kernels differ by five instructions, while their represented
material constructors, full-size copies and lifetimes agree. This intermediate
code precedes device-specific compilation and does not locate the hardware
failure. A full-context `-fno-strict-aliasing` control removes every actual
`!tbaa` and `!tbaa.struct` attachment, but reproduces all original outputs and
80 typed vertex slots exactly, including the same incorrect material values.
All four native commands complete; both physical-restoration checks fail.
Other alias mechanisms remain enabled. The rejected control's receipt is
`84486de21a79d1fd68afc164189c2a9e8a5639fcdf019a6109ca69f2cbb0e320`.
That control does not establish a compiler workaround.

A subsequent sole `-Os` to `-O1` change in both original full shader contexts
restores exactly the 32 material and 96 physical-response words. All other
368-word outputs and all 80 paired typed vertex slots match their respective
controls; all twelve nonzero physical records and all ten native/report commands
pass. Original failing and positive binaries first reproduce their entire output
and scratch buffers. The actual AIR loses `optsize` while preserving the target,
layout and strict floating-point options. This qualifies a compiler-mode
workaround candidate, without identifying a compiler defect or a device-code
mechanism. The runtime receipt is
`c418c2d575db627c884af34c77e04ba64103d90af6aa21f71e9dd4fe3a37b15c`.
Current-source regression, generated-path, scene-quality and performance
qualification remain separate requirements. That integration limited
`-O1` to the full renderer translation unit and its matching probe.

The current renderer-23 public regression preserves the full original context,
including its generated path and trailing PDF queries. A separate read-only
kernel extracts 220 explicit scalar fields from each of 40 stored vertices;
scratch bytes must remain unchanged across that readback. Compiling this same
source with `-Os` reproduces all 2,944 original failing output words and all
8,800 typed fields, with only the declared renderer-21 to renderer-23 metadata
translation. Compiling with the production `-O1` option restores exactly the
96 physical words and 32 nested reflectances. Every other output and typed field
stays exact. Both command buffers complete with API validation enabled and GPU
validation disabled; input immutability, guards and diagnostic inventories pass.
The paired qualification receipt is
`4246f690e545cdde48ace7b65d26c7d3de66e7e73d08aa17f2f5203402b09fd5`.

`metal.canonical_full_context` always requires the correct physical result.
Its 128 literal physical words expand to 192 strategy values and come from the
independently retained working renderer-21 positive context, authenticated before
the current run. They are a fixed M4 Max/toolchain regression witness, not a
cross-device accuracy oracle. The whole-output negative/positive qualification
above separately establishes that this public port retains the original failure.
This admits the material workaround; generated-path, scene-quality and final
performance acceptance require separate current-source evidence.

The fresh renderer-23 generated-path run uses the completed Release build at
`db18645`, with four freshly compiled libraries and the same 37 fixtures. All
296 context/sample rows and 148 captures are retained. Every one of the 388
observed continuous-path strategy records has the common rational density family;
the matched inverse-density control agrees in 81 of 388 records. The largest
observed hardware-weight discrepancy is `1.28e-7`. Each arm checks 7,668 coating
lanes, with exact within-arm Result/Splat controls and no semantic failures.
Successful finalization covers 399 finite arrivals, 107 environment escapes and
336 count-one cases. Delta, capacity, zero-throughput, tilted-normal and finite
PT controls also pass. The retained runtime receipt is
`002a6c93664248baa845a0a6691e5be9880acd724d91ce37b1e496c5a8a6c2fd`.
These are observations of tagged strategy records, not independent paths or a
proof of unbiasedness. The host auditor does not independently reconstruct
eligibility to require a family tag for every eligible path; no actual omission
was found. Discrete-delta strategy enumeration and broader feature coverage
remain outside this fixture. Historical renderer-21 counts above are separate.

Fresh renderer-23 day/night sanity images at 320×170 and 64 spp have complete
checkpoints, finite raw/authored-clamped FLOAT output and 128 completed sample
commands each. Their observed render loops are 8.492 and 13.039 seconds with API
validation enabled. The day observation exceeds the fixed 7.779-second target;
neither run is warmed repeated performance acceptance. Visible sampling noise
remains, and fresh quality bounds and final-resolution images are still required.

## Packet density and origin validity

Renderer 22 preserves a layered PDF's validity through the packet adapter,
forward/reverse evaluation and MIS consumers. Invalid values cannot become
legal zero-support densities through coincident-edge or black-contribution
early exits. The sampled joint throughput, physical scattering function and
random draws are unchanged. GPU checks cover seven packet PDF cases and 32
actual density-conversion cases; public PT and BDPT requests reject a finite
coating eta whose optical square overflows, while ordinary eta succeeds.

Outward ray-origin rounding now chooses the strict neighbor among finite
binary32 normal values and signed zero. In particular, departing zero produces
the smallest normal value in the requested direction, rather than a subnormal
outside the supported traversal domain. Tangential coordinates remain unchanged;
nonfinite inputs and outward overflow still fail. CPU checks cover 48 boundary
queries, and the actual GPU helper covers 288 axis/sign/untouched-coordinate
queries.

All 32 fixed public area-emitter requests succeed with the fix. Restoring only
the old rounding code in the matching shader snapshot causes all 16 zero-plane
requests to fail with invalid-hit diagnostics; all 16 translated controls still
succeed. Every request's outcome is retained before assertions, and failed
requests publish no partial samples or splats. This is a numerical-domain repair,
not a variance or speed result. Renderer-21 checkpoints are explicitly rejected.

## Host cost and timing instrumentation

`--timings PATH` adds opt-in JSON measurements for PBRT render commands. Nested
host phases report exclusive and inclusive integer nanoseconds. Completed GPU
command intervals are separate observations that overlap host waiting; they
must not be added to exclusive host time. Missing or invalid GPU timestamps are
reported explicitly. Instrumentation does not enter shader parameters, scene
identity or checkpoint payloads.

Sampling the published host identified canonical scene hashing as the dominant
previously unnamed startup work. The implementation now buffers canonical
serialization, batches eligible little-endian primitive arrays, and uses ARM
SHA-256 instructions where available, with the portable implementation retained.
Lengths, tags, ordering and floating-point bit patterns remain part of the same
canonical stream. Buffered integer encoding avoids repeatedly aliasing the
buffer's own write counter.

Native and forced-portable SHA tests cover known digests, padding boundaries,
unaligned input, irregular chunks, copied state and finalization behavior.
Identity fixtures exceed the buffer size and include shared images, signed zero
and nonfinite payload bits. Old/new serializer controls preserve the digest.

Four completed renderer-20 controls (day/night, instrumentation off/on) preserve
all 11,750,400 Film bytes and every raw/clamped FLOAT component. Each enabled
run reports all 896 expected GPU intervals. A later isolated hashing control
reduces the identity phase from 23.5895 to 7.4550 seconds with the same digest.
That control deliberately has no adjacent shader library and exits at GPU
preparation; it is not a successful render or a full-process speedup measurement.
Repeated complete-process and render-loop comparisons are still required.

## Isolated traversal experiments

A completed 32-render experiment uses frozen renderer-20 shader sources and the
same optimized hashing/timing host in every arm. This isolates execution changes
from the renderer-21 coating correction. Each variant has a separate baseline,
one warm-up per arm and three alternating measured pairs per scene, at
320×170/64 spp with seed/material zero. API-enabled arithmetic and adversarial
geometry checks precede all API-disabled timing runs.

| Variant | Scene | Baseline GPU seconds | Variant GPU seconds | Change |
| --- | --- | ---: | ---: | ---: |
| Remove duplicate internal source validation | Day | 32.5646 | 31.4671 | -3.37% |
| Remove duplicate internal source validation | Night | 47.6043 | 46.0983 | -3.16% |
| Keep fast triangle solver out of line | Day | 32.4451 | 32.6841 | +0.74% |
| Keep fast triangle solver out of line | Night | 47.5322 | 48.0331 | +1.05% |

All 32 complete Film payloads and 64 raw/clamped FLOAT planes are exact against
their frozen controls. All 896 sample-command intervals per render are valid.
The first variant also lowers measured process means from 50.2004 to 49.0984
seconds by day and 65.7062 to 64.2792 seconds by night. Neither experiment closes
the remaining speed target; the out-of-line variant offers no measured benefit.
These are fixed-workload diagnostics, not equal-time quality comparisons.

The execution receipt SHA-256 is
`9dae01ba9df6c42bd54b8eee856ea7522615abef90a9d6539175b01d91f1ed45`;
the complete paired statistics and host phases are in the retained summary,
SHA-256 `585e9a886af21d06ba35a1c6eb78c01dc497b755e0a1817e7db948cd9bf770e5`.

A later conservative triangle-miss filter is rejected. Its scalar error bounds
attempt to certify the same valid miss as both existing finite Pair solvers;
uncertain cases use the unchanged original solver. Standalone checks cover
32 results under strict and strict-Os flags, including two actual certifications
per configuration. The complete candidate library then passes the existing
arithmetic/witness, 40 transformed-ray and 463 native-hint dispatch checks.

Four renderer-22 scene controls on M4 Max at 320×170/64 spp preserve every Film
byte and raw/clamped FLOAT component against both the published reference and
the opposite arm. With API validation enabled, observed GPU time rises from
32.4009 to 43.0652 seconds by day and 47.4768 to 62.6249 seconds by night.
These single controls are not a warmed performance estimate; they provide no
reason to adopt this implementation or repeat its timing series. These observations
do not measure register pressure or spills. The execution receipt is
`2772988525ae19d1d01b7f960750f6912efca71835f6c298192647781f3bf2de`.

A separate instrumented day prefix (samples 0–7 with total-sample setting 64)
records 7,323,154 triangle-solver calls. The filter certifies 1,174,705 misses,
**16.04% of calls**, all already handled by the ordinary fast solver. It saves
no extended solves and certifies none of the 1,459,150 native-hint validations.
Both pipelines preserve all captured path outputs, original workspace bytes,
Film payloads and raw/clamped FLOAT components. This census identifies limited
work avoidance in that observed prefix; it does not measure uninstrumented
traversal frequency, attribute elapsed time or establish night-scene rates.
Its execution receipt is
`0c21b463da46383202d9e0bb1dfb5ad9c759f3918572d00086a679641c677fe8`.

A subsequent explicit Pair-times-scalar specialization preserves the generic
fallback and passes 668,296 differential cases under both strict and strict-Os
GPU compilation, plus the three existing traversal admissions. All four
day/night scene controls preserve Film and FLOAT output exactly. Observed GPU
time changes by only −0.56% day / −0.63% night; startup differences confound the
whole-process comparisons. Adoption and repeated timing are on hold because
these single API-enabled controls establish no substantial improvement.
The scene receipt is
`161fdfe05f192285a2a83b80e74eeba7e343af79d7e88d67fb2235641886349a`.

An Instruments Performance Limiters capture of published renderer 22 completes
for a day render at 320×170/8 spp. Its Film and FLOAT outputs exactly match the
unprofiled control. Analysis resolves 24,989,678 raw counter records into 85
separate series. During the inferred rendering windows, unweighted sample means
show 1.10% compute occupancy and a 99.71% occupancy-manager target. These are
device-wide observations, not exclusive renderer counters or duration-weighted
averages. The source-supported mapping selects the last 112 command buffers;
their span sum differs from the application's interval sum by 1.11%.

The measurements motivate testing dispatch underfill: the packet driver currently
limits each one-sample dispatch to 4,096 pixels, although the existing 256 MiB
workspace permits 9,992 lanes at the scene's depth. They do not establish a
register/spill bottleneck or prove that larger dispatches will improve speed.
The completed counter-analysis receipt is
`a5e93605851a1456c912e12c3b9681c2457a9faff64015bd45938fb821dcb975`.

A host-only pilot replaces the 4,096-pixel packet cap with the existing workspace
capacity. The shader library, lane layout, samples and accumulation order remain
unchanged. Both newly linked controls match published renderer 22 exactly; all
four day/night Film payloads and raw/clamped FLOAT planes match their controls.
Each frame retains 3,481,600 sample owners, while sample commands fall from 896
to 384. Maximum output allocation rises from about 105 MiB to 256 MiB within the
existing budget.

| Scene | 4,096-lane GPU seconds | 9,992-lane GPU seconds | Process seconds, before → after |
| --- | ---: | ---: | ---: |
| Day | 32.510 | 15.393 | 53.622 → 32.613 |
| Night | 47.759 | 22.306 | 65.746 → 39.595 |

These are single API-enabled pairs at 320×170/64 spp, with no declared warm-up;
they motivate further work but do not establish a repeated speedup, isolate
occupancy from submission/cache effects, or close the time-to-quality target.
The complete execution receipt is
`b45dbf401cbab8acb9fd4f9fe18bfd219f9a9b6334715a6664f4e8465a3e8272`.

A second paired host-only experiment increases the packet payload ceiling from
256 MiB to 1 GiB, with the same shader and sample order. At depth five this raises
capacity from 9,992 to 39,969 lanes and reduces the measured commands from 384 to
128. The existing 16 GiB scene preparation limit and native device/allocation
checks remain in force. Both original and candidate frames reproduce every
published Film byte and raw/authored-clamped FLOAT component. Observed GPU time
falls from 15.395 to 6.713 seconds by day and 22.325 to 10.719 seconds by night.
The complete receipt is
`63775ebccb4e0fb4fbc53ad08661dcd1f842c7a8f521a98a561bad156255e2e7`.

With that same capacity, initializing only each lane's Result and reserved Splat
records plus the final buffer guard reduces host setup work. Path construction
writes vertices before exposing them through its counts; unused Vertex storage
is no longer poisoned. Missing-output sentinels and guard checks remain, but this
does reduce diagnostic coverage for erroneous internal Vertex reads. The change
does not claim equivalent malformed-shader fault detection. All six image audits
against published, paired and prior-capacity controls pass exactly.

| Scene | Full-fill setup (s) | Prefix-fill setup (s) | Full-fill loop (s) | Prefix-fill loop (s) |
| --- | ---: | ---: | ---: | ---: |
| Day | 0.756 | 0.128 | 8.201 | 7.590 |
| Night | 0.777 | 0.134 | 12.276 | 11.596 |

These remain single API-enabled pilots, not warmed repeated admission. The
integrated policy grows the default 256 MiB payload only with a known preparation
estimate and available scene/device headroom, charging three bytes of estimated
storage per additional payload byte, up to 1 GiB. Native maximum buffer size
also bounds advertised capacity. Unknown estimates decline optional growth;
scalar scheduling is unchanged. This extends preparation accounting rather than
establishing a whole-process peak-memory bound. Integrated regression and final
timing/quality checks remain separate. The integrated host change passes all 227
Release tests, the selected Metal suites with API validation, and native tiling
at the maximum supported path depth. Fresh integrated day/night controls retain
all Film/FLOAT bits and produce 128 completed sample commands per frame. Their
observed loops are 7.593 and 11.604 seconds; no repeated-speed claim follows.

The renderer-23 daytime pilot at `6828383` uses one warm-up per arm and five
alternating measured pairs at 320×170/64 spp, with validation disabled and no
overlapping renderer/compiler work. All twelve captures precede all image
audits; every same-arm Film byte and raw/clamped FLOAT component matches its
prebound reference. The mean render loop is 8.471438 seconds versus fast-v5's
7.103950 seconds (ratio 1.192497), failing both the 1.10 ratio and 7.779024-second
ceiling. Mean whole-process time is 24.413554 versus 52.189863 seconds. All
captures and failures of the speed gates are retained; this daytime pilot does
not replace the final two-scene timing or quality protocol. Its receipt is
`18fa171737d59ef25fd57859f849d2a73879afe980a11e2a82199071db1ca842`.

Renderer 24 consequently limits canonical stored-density finalization to BDPT.
PT consumes fresh represented-geometry densities for emission and NEE MIS, not
the stored forward/reverse factors. Both arrival sites now skip that unused
work in PT; zero-initialized stored fields remain unused. Sampled forward and
reverse PDFs, their area conversions, and all densities actually consumed by PT
retain their existing validity checks. The direct finalizer and BDPT recurrence
are unchanged. This changes the numerical error domain: PT may accept a path
previously rejected solely by an unused canonical query. It is not a universal
renderer-23 equivalence claim. The target physical model, joint throughput and
sampling remain unchanged.

At `d7d10b2`, eight focused Release tests pass with API validation, including
the full material context and invalid sampled-density checks. The generated-path
check passes all 373 observed continuous strategy records; its inverse control passes
81. Literal PT rows retain zero unused stored densities and do not manufacture
virtual BDPT families. No natural invalid-unused-query witness has been found.
Both 320×170/64-spp day/night controls retain every renderer-23 Film byte and
raw/clamped FLOAT bit. The fixed daytime series still fails both speed gates:
8.213380 seconds versus fast-v5's 7.096472 (ratio 1.157389), with whole-process
means of 24.112730 and 52.109692 seconds. All twelve same-arm image audits pass.
The timing receipt is
`8b3420e49bfbf414f4838df94780a3864f3e8c130357308b2df3c242f37c2b9a`.

A subsequent isolated compiler control changes only the two full-context
translation units from `-O1` to `-O2`, preserving strict FP, source, host and
ABI. Both actual AIR exports show eliminated redundant loads and simplified
control flow, rather than only metadata changes. Fresh captures match all
2,944 output words and 8,800 typed material fields exactly, including the prior
physical regression, with API validation enabled. Its receipt is
`311db50532b34e311b114b3b8f5688a084bd79fd10c6280c9801492c62d62421`.
The subsequent production integration at `5516fc4` passes seven of eight focused
Release tests, but `metal.packet_render` exceeds its unchanged 300-second
deadline. Metal's compiler service remains CPU-active during that wait; the
specific blocked pipeline call or optimizer pass is not established. The material
probe uses a different entry point from the production packet kernel and cannot
qualify that kernel's compilation. Both the timed-out test and its observed
compiler process have exited. This rejects `-O2`; no scene or speed result is
claimed for it, and the working source restores `-O1`. All other shader
optimization modes and renderer contract 24 remain unchanged.

The next execution-only change binds optional Metal function constant 20 to the
saved scene parameter's PT/BDPT truth value. All fourteen integrator-dependent
shader conditions use the same predicate, including environment proposals and
MIS validity checks. The saved choice is immutable across sample requests.
When the constant is unset, standalone probes retain the original runtime
predicate. This uses Apple's documented
[function specialization](https://developer.apple.com/documentation/metal/using-function-specialization-to-build-pipeline-variants)
and the optional-constant query in section 5.8.1 of the
[Metal language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf).
It changes no model, random draws, arithmetic formulas, storage layout or
checkpoint contract. Current-source material, generated-path, exact day/night
and repeated-speed qualification remain pending; no compiler elimination or
performance claim follows from the source change alone.

## Quality protocol

The separately versioned v2 runner requires 64 fresh independent replicates per
scene and arm. It retains all 48 simultaneous variance/tail endpoints and all
36 raw RGB mean safeguards, with the inherited thresholds and whole-image
resampling rules. It validates the actual library beside the resolved executable,
as well as the executable and other frozen inputs. The draft manifest cannot
launch until model, source, seed ledger and artifacts are bound.

The reduced day investigation uses the whole predeclared soffit ROI and a fixed
polar/concentric by Halton/independent factorial. Selected-owner observations
remain exploratory. Neither additional replicates nor a changed sampling map is
evidence of improved variance until the complete planned comparisons finish.

## Current regression checks

Renderer-23 integration at `050812e` builds successfully and passes 237 Python
tests. The first full Release run passes 227 of 228 tests, including all Metal
tests, with no availability skips. Its sole failure is an identity-test golden
that still encodes renderer 22. The retained independent Python serializer first
reproduces both renderer-22 identity hashes, then derives renderer-23 hashes for
the same 3,766-byte empty and 67,499-byte populated fixtures; only the tagged
renderer field changes. Both updated identity fixtures pass in the focused
Release rerun. This test-only correction changes no rendering code. Published
`6828383` also passes all 228 mandatory Debug tests. The generated-path and
day/night sanity results in that publication apply to renderer 23; fresh
renderer-24 evidence is reported separately above. Renderer-24 wire and identity
goldens are derived independently
after reproducing all three prior renderer-23 hashes; only the renderer field
changes.

CI run `44931592050` passes macOS tests, formatting, Clang-Tidy and CodeQL. Linux
passes all 164 Debug and 164 Release tests, then reaches its ten-minute job
limit during ASan/UBSan tests without a reported assertion failure. The workflow
now runs the three presets as separate jobs with independent ten-minute limits
and `fail-fast: false`. The required Foundation Gate still requires every job
to succeed; no tests or failure checks are removed.

Historical renderer-21 validation passes 227 Release tests (151 non-CLI tests with Metal API
validation and 76 CLI tests in their default environment), all 227 ASan/UBSan
tests, all 164 portable tests with Metal disabled, and 237 Python tests using the
project environment. The first Release run exposed a stale renderer-20 binary
checkpoint checksum. Independent Python serialization reproduces the old golden
and the new renderer-21 golden; the corrected test passes and explicitly rejects
authenticated renderer-20 checkpoints. The initial system-Python test invocation
lacked the pinned `emoji` dependency; the complete rerun uses the project venv.

The renderer-22 PDF/origin integration on parent `1902d2a` passes all 227 Release
tests, all 164 portable tests, the six affected CTest targets with Metal API
validation, and all 237 Python tests. Clang-Tidy 18 passes on the four affected
C++ test translation units. Its independent checkpoint serializer reproduces the old
renderer-21 goldens before deriving the new renderer-22 values. The first focused
run exposed missing texture-frame metadata in the new coated-material fixture;
preparing the actual per-instance frames repairs the fixture and its rerun passes.
An initially misconfigured full-suite run was interrupted because API validation
must be unset for CLI output-contract tests; the complete run uses that policy.
These checks do not include the unadopted canonical-density prototype.
Fresh renderer-22 day/night progress renders at 320×170 and 64 spp complete with
finite raw/clamped FLOAT outputs and independently decoded checkpoints. Their
display previews use standard sRGB encoding without denoising; they do not
establish final-resolution quality or convergence.

Published renderer-22 revision `3c52b42` additionally passes all 227 Debug
pre-push tests, all 227 ASan/UBSan tests, all 65 Debug Metal tests with API
validation, and static analysis of both production Metal adapters. All jobs in
[CI run 44900321433](https://github.com/rlefko/rypath/actions/runs/44900321433)
pass. Final scene-quality/performance acceptance remains separate; these checks
do not resolve the cross-strategy density issue described above.
