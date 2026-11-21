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
pipeline variants per arm, observed material fields and within-arm sample/splat
controls pass; the material checks cover 7,952 coating lanes per arm. This
supports the density repair but is not a scene-quality result.

Adoption remains blocked by a separate constructed-context failure: four
nonzero-base cases lose all four nested reflectances in two stored vertex
copies, changing twelve strategy lighting results. The original binary
reproduces exactly. Enabling GPU validation restores precisely those 32 material
words and 96 lighting words, with no reported fault; all other captured words
remain unchanged. Instrumentation changes execution and cannot establish the
cause or serve as a production repair. The canonical-density prototype remains
outside the renderer-22 integration described below.

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

Debug pre-push checks, GitHub CI and final scene-quality/performance acceptance
are separate publication gates. Passing these regression checks does not resolve
the cross-strategy density issue described above.
