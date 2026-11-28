# Enhanced execution investigation

The native four-scene screen confirms a regression when opting into the
Enhanced estimator at `9d34076`. Main `5194f8d` remains the retained baseline.
This is unfinished optimization work; neither the 20 FPS Cornell/Prism target nor
the 10 FPS Pavilion target is met. The complete paper and scene-quality benefits
are not qualified by execution tests.

## Matched native screen

All arms use native 1920×1080 input/output, one fresh four-wavelength visible-CDF
hero path, zero mutations, signed temporal reconstruction, native intersection,
query shadows, default AS builds, packed one-event execution, capacity 65536,
quantum 8 and group 32. Guide and exact temporal caches are enabled; motion and
selected-null compaction are disabled. Seed 61002 uses two warmup frames and
three measured complete-frame CPU intervals per arm. Startup and export are
excluded. This short screen is not sustained, moving-camera, p95 or time-to-quality
qualification, and it must not be compared directly with one-mutation results.

| Scene | Main contribution-prefix | Same contribution on `9d34076` | Enhanced, paired Gaussian and suffix cache |
| --- | ---: | ---: | ---: |
| Cornell | 759.17 ms | 755.71 ms | 956.00 ms |
| Prism | 739.21 ms | 748.77 ms | 1463.74 ms |
| Pavilion day | 2006.16 ms | 2005.24 ms | 2753.79 ms |
| Pavilion night | 1992.22 ms | 2000.78 ms | 2801.95 ms |

The unchanged contribution domain stays within about 1.3% of main. Enabling
Enhanced with reciprocal pairing and suffix caching disabled already adds about
33%, 90%, 34% and 36%, respectively, relative to that same-revision control.
Gaussian pairing adds about 2% over reciprocal Enhanced with its suffix cache.
These domains have different roulette and mapping laws; this comparison does
not establish equal image quality or an execution-only estimator replacement.
All 20 jobs and 100 frames completed with finite native final captures.

## Actual work and shader resources

A separate instrumented 12-job screen uses two warmup frames and one measured
frame per scene and arm. Its successful sample-2 packet-step counts are:

| Scene | Contribution | Enhanced reciprocal, suffix off | Enhanced paired, suffix on |
| --- | ---: | ---: | ---: |
| Cornell | 17,715,334 | 12,775,549 | 11,515,749 |
| Prism | 21,119,717 | 20,720,559 | 20,617,072 |
| Pavilion day | 22,182,832 | 19,135,708 | 19,094,356 |
| Pavilion night | 21,792,043 | 19,233,399 | 19,191,720 |

Enhanced takes longer despite fewer calls in every scene. The shade stage is
the largest listed instrumented interval in all 12 jobs. Those intervals include
traversal, BSDFs, hybrid geometry and suffix operations; encoder instrumentation
changes execution and intervals can overlap, so they are not pure shading cost,
wall-time fractions or native FPS.

Xcode profiling of current-runtime 320×180 Prism captures reports 192 allocated
registers for both main shade variants, with 4.84 KiB of spill storage for
contribution and 8.58 KiB for Enhanced. This associates Enhanced with greater
temporary storage pressure, not proof that spills alone cause the regression.
The native-resolution Enhanced replay failed with a replayer bus error; only the
smaller successful profiles support these resource observations. Their cost
shares are not native-resolution performance measurements. Apple's
[compute guidance](https://developer.apple.com/videos/play/tech-talks/10580/) and
[GPU profiling guidance](https://developer.apple.com/videos/play/tech-talks/111374/)
motivate reducing live state and measuring resulting allocation and occupancy.

## Isolated execution experiments

Two single-annotation outlining experiments retain the original operation bodies.
A third explicitly gathers a 112-byte completion view instead of copying the
292-byte Enhanced state again after the event; its completion formulas and
persistent state remain unchanged. Each experiment uses separate frozen runtimes
and ABBA order, with two warmup and three measured
frames per process under the native settings above.

| Change | Cornell mean change | Prism mean change | Decision |
| --- | ---: | ---: | --- |
| Outline hybrid geometric edge helper | −0.78% | −2.21% | Insufficient to recover regression |
| Outline connection-certification helper | +5.08% | +3.56% | Rejected for speed |
| Gather smaller completion view | +0.92% | −1.76% | Mixed, insufficient recovery |

All Cornell/Prism final raw/reconstructed FLOAT comparisons are bit-exact,
including baseline/baseline and candidate/candidate controls. The edge experiment's
Pavilion night controls also match, with a 1.26% lower mean. Pavilion day fails
its strict control: the final unchanged baseline differs at one pixel from the
first baseline and both candidates. Its cause is unresolved; this is not evidence
of an outlining-induced error or an accepted numerical tolerance. The original
Pavilion harness failure from noncanonical dependency paths is retained separately
from its corrected fresh study and this numerical control failure. No
experiment is adopted.

Two further native four-scene ABBA experiments compare integration `3dc8b9d`
with isolated execution changes under the same settings:

| Change | Cornell | Prism | Pavilion day | Pavilion night |
| --- | ---: | ---: | ---: | ---: |
| Aggregate queue reservations within SIMD groups | −0.32% | −1.00% | −7.41% | −0.36% |
| Avoid repeated preparation inside fresh reservoir selection | −2.42% | +0.01% | −1.00% | −0.82% |

These are descriptive changes in mean frame time, not qualified speedups. The
queue study's daytime mean includes a substantially slower first baseline run.
Both studies complete all 80 frames with finite captures, but fail strict image
controls. The queue study repeats the same daytime pixel difference across
libraries; its A/A and B/B controls match. The reservoir study repeats that
daytime difference and adds two differing raw night pixels, including a B/B
failure; reconstruction spreads that night difference across 62 pixels.
Neither candidate is adopted. Cornell/Prism final controls remain exact in both
studies, but their small or absent timing gains do not recover the regression.

A separate Pavilion-day test shares an acceleration structure within each A/A
or A/B contrast while keeping shader programs and mutable session state separate.
All ten raw XYZ comparisons are exact across 62,208,000 finite float pairs.
Its wrapper failed a device-name metadata comparison; an independent audit
passed with only the previously identified name-only correction, preserving the
original failure. This diagnostic does not prove the cause of the separate-AS
differences or clear either failed performance study.

## Lower-resolution quality screen

A fixed diagnostic at `3dc8b9d` compares independent PT, Enhanced with zero
mutations and Enhanced with one mutation. Cornell/Prism use 320×180; Pavilion
uses 320×170. Each arm has eight independent seed/session groups and 32 total
frames; warm summaries exclude the first four frames. References use current
visible-hero PT, with eight independent 256-SPP batches totaling 2048 SPP.
Area uses paired Gaussian reuse, prefix replay and suffix caching; signed temporal
reconstruction uses the conservative spatial variance policy. Motion, compaction
and duplication diagnostics are off.

All four jobs, 3072 evaluation frames and 424 finite FLOAT images complete.
Independent review checks the bindings, accounting, aggregation and fixed-region
endpoint analysis. Completion establishes usable evidence, not quality acceptance.
The table gives signed linear-RGB MSE against the finite reference: mean of eight
warm-frame seed means ± standard error across those means. Correlated frames
and spatial pixels are not additional independent replicates.

| Scene | Arm | Raw MSE ± SE | Reconstructed MSE ± SE |
| --- | --- | ---: | ---: |
| Cornell | PT | 0.053682 ± 0.000346 | 0.00272235 ± 0.000113 |
| Cornell | Enhanced, 0 mutations | 0.0189578 ± 0.000298 | 0.0198204 ± 0.000525 |
| Cornell | Enhanced, 1 mutation | 0.0195118 ± 0.000221 | 0.0146241 ± 0.000447 |
| Prism | PT | 0.261897 ± 0.00190 | 0.0180322 ± 0.000408 |
| Prism | Enhanced, 0 mutations | 0.0553819 ± 0.00425 | 0.104921 ± 0.00886 |
| Prism | Enhanced, 1 mutation | 0.0557436 ± 0.00410 | 0.102237 ± 0.00812 |
| Pavilion day | PT | 4.03583 ± 2.57 | 1.59747 ± 1.25 |
| Pavilion day | Enhanced, 0 mutations | 1.10382 ± 0.772 | 0.848167 ± 0.786 |
| Pavilion day | Enhanced, 1 mutation | 1.29325 ± 0.813 | 0.468991 ± 0.415 |
| Pavilion night | PT | 38.7148 ± 22.2 | 2.85327 ± 2.17 |
| Pavilion night | Enhanced, 0 mutations | 0.602171 ± 0.100 | 0.758789 ± 0.100 |
| Pavilion night | Enhanced, 1 mutation | 0.547265 ± 0.0967 | 0.584455 ± 0.0921 |

Reference MSE noise floors are respectively 0.0000248126, 0.000132460,
0.0302598 and 0.0498214. They are retained separately, not subtracted or clipped.
Cornell/Prism raw means improve relative to PT, while their reconstructed means
are worse than reconstructed PT. Enhanced zero-mutation reconstruction also
worsens its own raw mean in both scenes. The conservative policy does not
explicitly model Area temporal covariance; correlation is a hypothesis for
investigation, not an established cause. A same-input temporal/spatial filtering
ablation is needed to isolate the reconstruction interaction.

Pavilion has substantial seed variation, especially in daytime results and
independent PT's bright outliers. The fixed-region analysis preserves signed
values, raw positive tails and finite-reference sensitivity. It does not establish
highlight repair. Its sample-31 endpoint contrasts are a different population
from the warm-frame means above.

There is no older contribution-domain Area quality arm. This screen cannot
isolate Enhanced's quality benefit over that implementation or establish that
quality compensates for the native slowdown. Reduced-resolution measurements
do not establish native FPS, convergence or time to equal quality.

## Reproduction and validation scope

Local artifacts under `.cache/validation/hero-performance-implementation-001/`
retain frozen runtimes, exact commands, source/build/scene hashes, settings,
per-frame accounting and raw/reconstructed FLOATs. Relevant studies are
`enhanced-regression-native-001`, `enhanced-event-count-native-001`,
`profiling/enhanced-regression-small-capture-001`,
`enhanced-edge-outline-native-001`, `enhanced-edge-outline-pavilion-recovery-001`,
`enhanced-certification-outline-native-001`, `enhanced-finish-view-native-001`,
`enhanced-simd-queues-native-001`, `enhanced-fresh-transaction-native-001`,
`enhanced-shared-as-pavilion-001`, `enhanced-quality-screen-001` and
`enhanced-quality-screen-001-analysis`,
`enhanced-followup-validation/typed-serializer-release-001` and
`profiling/typed-qualified-event-001`.
Independent review receipts
are in `enhanced-followup-research/`. These large local artifacts are excluded
from Git; this record does not claim that they accompany a clone.

The counter implementation passes `metal.wavefront_frame`,
`metal.wavefront_queue`, `frame.gpu_diagnostics`, `metal.hero_event_counts` and
`metal.event_count_unsupported_json` in Release on M4 Max. Controls exercise
capacity/quantum changes, initialized-complete paths, transparent boundaries,
null admission, carry/overflow, cached passes and unsupported JSON output.
Actual profiled/unprofiled frame controls preserve raw output. Native diagnostic
evidence covers 36 completed frames, with per-leaf accounting independently
checked. These checks validate measurement, not a rendering speedup.

The completion-view prototype passes its 24-case full-state/view GPU differential
and the queue, selected-null compaction and sequential suffix controls. Its
native screen completes 40 frames and preserves all 24 final raw/reconstructed
FLOAT comparisons. These controls establish the bounded execution result; the
mixed timing prevents adoption as a performance repair.

The integration build at `3dc8b9d` passes the required Debug script, including
all 311 tests, and a full Release build. This is mechanical and bounded numerical
validation; it does not overturn the performance or reconstruction findings.

The isolated complete material-event specialization probe at `93c9ef5` passes
its bounded diffuse correctness gate after replacing typed pointer walks with
explicit field serialization. Independent raw-buffer readback agrees with the
serialized next-slot, observed-count and selected-XYZ fields. Generic and typed
entries preserve exact canonical state, mutable arenas and guards at primary,
pivot and interior events for all 16 lanes. The complete runtime probe also passes
its positive-term, earlier-label zero, forced replay and appended-occluder controls.
The original failed receipts and captures remain preserved; the repair does not
establish a unique compiler cause. This fixture disables suffix caching and does
not qualify mixed materials, sparse masks, production queues or performance.
The subsequent paired capture passes its strict pre/post-capture event checks.
Xcode reports 192 allocated registers for both complete-event entries, with
16.75 KiB spill storage for generic current versus 14.72 KiB for typed diffuse
current (about 12% less). These are strict `-O1` probe kernels at 16 lanes and
three event states, with generic historical state retained; their absolute
allocations are not comparable to the production shade profile above. The
capture wrapper rejected three Metal-created internal file aliases; independent
review inventories the 26 regular files and three same-directory aliases while
preserving the original wrapper failure. Successful replay and resource inspection
support a production specialization experiment, not a native-frame speedup.
Production adoption still requires lower complete-frame cost and preserved
transport, including unsupported charts, coatings and spectral behavior.
