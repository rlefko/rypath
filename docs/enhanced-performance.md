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

## Reproduction and validation scope

Local artifacts under `.cache/validation/hero-performance-implementation-001/`
retain frozen runtimes, exact commands, source/build/scene hashes, settings,
per-frame accounting and raw/reconstructed FLOATs. Relevant studies are
`enhanced-regression-native-001`, `enhanced-event-count-native-001`,
`profiling/enhanced-regression-small-capture-001`,
`enhanced-edge-outline-native-001`, `enhanced-edge-outline-pavilion-recovery-001`,
`enhanced-certification-outline-native-001` and `enhanced-finish-view-native-001`.
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

Complete material-event specialization is the next bounded experiment.
It requires actual compiler resource evidence, equivalent
event/frame outputs and lower complete-frame cost before adoption. Unsupported
charts, coatings and spectral effects remain part of the renderer's contract.
