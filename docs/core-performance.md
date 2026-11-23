# Core renderer performance

This follow-up targets execution cost after the
[golden-pipeline qualification](pipeline-qualification.md) missed the native
1080p, 20–30 rendered FPS objective. Raw spectral transport and the signed
temporal reconstruction model remain separate and unchanged. Area/MCMC estimator
tuning is deferred; its earlier quality and speed failures remain open.

## Final static configuration

Exact guide reuse is enabled by default. Packed one-event execution, query
shadows and the existing native AS build policy remain the defaults; the other
policies remain explicit experiments. Simple SIMD counter aggregation is retained
under its mechanical-change exemption, without claiming a meaningful speedup.

The full-stack comparison uses clean Release source `54fa952` against the frozen
`b8dc9cc` executable, with stage instrumentation disabled in both. The baseline
includes the diagnostic infrastructure; this is not a new timing measurement of
`4b1cc65`. Native 1920×1080, one fresh independent sample and signed temporal
reconstruction are unchanged. The selected arm omits cache/packet/shadow/AS
controls and exercises its actual defaults.

All 36 runs complete (5,760 frames including warmup). The 36 final raw and
reconstructed image pairs are bit-identical; per-frame sample totals, logical
counters and history decisions agree. The table uses medians across the three
seeds. Reduction is the median paired p95 ratio; FPS is the median of each run's
measured-frame count divided by measured wall time, not a reciprocal median.

| Scene | Baseline median, ms | Selected median, ms | Selected p95, ms | Paired p95 reduction | Selected FPS |
| --- | ---: | ---: | ---: | ---: | ---: |
| cornell | 73.695 | 70.334 | 72.224 | 4.52% | 14.19 |
| many-lights | 53.578 | 50.251 | 51.593 | 5.51% | 19.88 |
| glass | 106.702 | 89.697 | 91.140 | 15.92% | 11.16 |
| defocus | 46.753 | 33.182 | 33.826 | 29.52% | 30.12 |
| media | 144.659 | 128.728 | 130.905 | 11.12% | 7.77 |
| coated | 347.977 | 315.482 | 318.130 | 9.64% | 3.19 |

Every individual seed/scene improves p95; the defocus target passes the 10% gate
and no other scene approaches the 3% regression limit. No static confirmation is
triggered. Only defocus passes p95 ≤50 ms in all three seeds; no scene passes
p95 ≤33.33 ms in all three. Its median throughput reaches 30.12 FPS, but that does
not establish a 30 FPS tail guarantee. The 20–30 FPS objective remains unmet
across the suite, especially for coated transport. Sustained results are separate.

## Sustained final controls

Seed 42001 uses 64 warmup frames, followed by 1,100 measured frames per Cornell
arm and 256 per coated arm. These counts were declared before execution from the
previous timing budget with margin. Each of the four runs exceeds both 240
measured frames and 60 measured seconds. The four final raw/reconstructed image
pairs are bit-identical, with matching per-frame accounting and history decisions.

| Scene | Arm | Measured frames | Measured seconds | Median, ms | p95, ms | FPS |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| cornell | baseline | 1100 | 81.053 | 73.709 | 75.258 | 13.57 |
| cornell | selected | 1100 | 77.383 | 70.311 | 71.870 | 14.22 |
| coated | baseline | 256 | 88.409 | 348.282 | 352.150 | 2.90 |
| coated | selected | 256 | 80.225 | 315.650 | 318.265 | 3.19 |

Sustained p95 falls 4.50% for Cornell and 9.62% for coated, consistent with the
three-seed static comparison. Neither scene reaches 20 rendered FPS. These
stationary controls do not establish sustained moving-camera performance.

## Measured stages

Opt-in [stage diagnostics](frame-rendering.md#opt-in-gpu-diagnostics) on the M4 Max
identify packet shading, surface-guide generation and spatial filtering as the
largest coated-scene intervals. The device supports encoder-stage timestamp
sampling, so instrumentation inserts encoder boundaries and changes scheduling.
These intervals describe that instrumented execution; they are not production
frame times and must not be added as an end-to-end GPU measurement.

The following diagnostic medians use one warmup and two measured frames per
scene, native 1920×1080, one fresh independent sample per pixel, signed temporal
reconstruction, seed 41001 and Metal API validation. These runs use compiled
revision `b8dc9cc` and precede subsequent portability and optimization commits.
The retained reports include the exact source/flags fingerprints.

| Scene | Shade, ms | Trace, ms | Surface guides, ms | Spatial passes, ms |
| --- | ---: | ---: | ---: | ---: |
| Cornell | 28.694 | 15.118 | 3.191 | 17.201 |
| Many lights | 18.477 | 8.427 | 2.671 | 16.541 |
| Glass | 46.506 | 13.793 | 15.789 | 18.498 |
| Defocus | 6.880 | 4.263 | 13.376 | 16.153 |
| Media | 80.489 | 17.556 | 15.402 | 18.704 |
| Coated packet | 271.011 | included in shade | 32.250 | 18.951 |

The six implementations address distinct costs:

- Optional stage timing and bounded frame capture expose scheduling costs.
- Exact surface-guide reuse skips guide work when all resolved current and
  previous camera/texture inputs match; fresh color and filtering still execute.
- SIMD aggregation reduces contention for simple reconstruction counters.
- Eligible native opaque triangle scenes can use a Boolean shadow intersector.
- Native AS experiments request faster intersection builds at BLAS, TLAS or
  both levels, retaining actual allocation/build telemetry.
- Packet experiments separate the 224-byte hot state from its full 1,008-byte
  prepared vertex, optionally split trace/shade, and fuse one, two or four
  semantic events before state publication. The prepared BSDF, random streams,
  contribution order and stopping rules are preserved.

See [reconstruction](reconstruction.md), [geometry](geometry.md), and
[wavefront execution](wavefront-rendering.md) for contracts, primary references
and numerical limitations. In particular, the opaque specialization retains an
existing native grazing/shared-edge miss; it does not claim compensated-profile
accuracy. Older shader libraries must explicitly demonstrate support before
opaque execution is reported.

## Qualification protocol

The hardware is the 40-GPU-core M4 Max with 128 GiB unified memory, running
macOS 26.6 (Darwin 25.6.0), Xcode 26.6 and the macOS 26.5 SDK. Performance uses
Release executables with Metal API/shader validation and capture disabled.
Correctness checks use Metal API validation and are reported separately.

Timing is isolated from our builds, GPU tests, profiling and captures. Captured
images are written after the measured frame loop. Unrelated desktop activity is
not terminated, and OS thermal warnings are recorded without inferring GPU clock
or temperature. Binary and metallib hashes, compiled-source snapshots and
production-equivalent review revisions are retained separately.

All comparisons keep native 1920×1080, one fresh sample, independent sampling,
native intersections, signed temporal reconstruction, capacity 65,536, quantum
eight and group width 32. The six scenes use the original qualification's
[fixture paths, generation recipes and hashes](pipeline-qualification.md#reproduce-the-inputs).
Structural changes require at least a 10% reduction in complete-pipeline p95 on
their declared target scene, with no clear regression above 3% elsewhere. Small
counter aggregation is exempt from the large-win requirement. Same-model raw
and reconstructed captures must retain exact pixel controls; matching final
captures alone do not establish per-frame raw XYZ equivalence.

Qualification uses seeds 41001, 41002 and 41003, 64 warmup frames and 96 measured
frames per arm. Baseline/candidate order alternates by scene and seed. Per-frame
summaries use lower nearest-rank medians and p95; seed aggregates use ordinary
medians. AS and packet policy screening uses the predeclared first seed, eight
warmup frames and 24 measured frames. Every attempted run is retained. One
predeclared confirmation is allowed when a paired median ratio lies within one
percentage point of a decision threshold or seeds straddle it, using seeds
41004–41006 with the full frame budget. Initial and confirmation results remain
separate; acceptance requires both the combined median and confirmation alone
to pass. Otherwise an inconclusive candidate retains the existing default.

Camera-motion timing is separate: four measured quarters hold the authored pose,
translate by 0.1 along world X, hold that translated pose, and cut back with a new
epoch. Each phase is summarized independently. The media fixture is excluded
from this motion driver because it cannot classify camera crossings of distinct
medium interfaces; its static qualification remains required. Motion timings
alone do not establish reconstruction quality. Sustained final Cornell and
coated controls must each include at least 240 measured frames and 60 measured
seconds.

Complete-pipeline wall time includes serial resident raw rendering and
reconstruction, including CPU gaps between submissions. It excludes process
startup, capture/output and viewer presentation. These are rendered-frame
measurements, not display or frame-interpolation rates.

The retained experiment plans, commands, reports, frozen binaries, independent
decoder audits and diagnostic tables live outside Git under
`.cache/validation/core-speed-001/` in the durable checkout. Component correctness
receipts remain in their respective worktrees. Component static and phased motion
qualification select exact guide reuse as the default. The fresh combined-stack
static and sustained results are reported above. Across the component screens,
rejected candidates, qualification and sole motion confirmation, the retained
record contains 340 runs and 39,832 frames (25,464 measured), with 340 exact final
image comparisons (170 raw and 170 reconstructed). The final selected-versus-baseline
studies account for 40 runs and 8,728 total frames. Component gains are not
multiplied into the final result.

For one defocus arm, after a supported Release build, use fresh output paths:

```sh
MTL_DEBUG_LAYER=0 MTL_SHADER_VALIDATION=0 MTL_CAPTURE_ENABLED=0 \
build/release/rypath-frame-benchmark \
  --scene tests/fixtures/pbrt-render/sampling-defaults/lens-texture.pbrt \
  --output defocus-41001-on.json \
  --width 1920 --height 1080 --frames 96 --warmup 64 \
  --seed 41001 --material-seed 0 --execution wavefront --intersections native \
  --capacity 65536 --quantum 8 --group-width 32 \
  --estimator independent --target-scale 1 --reconstruction temporal \
  --guide-cache on --capture defocus-41001-on-raw.exr \
  --reconstructed-capture defocus-41001-on-reconstructed.exr
```

Use cache off/on in the same executable for the component comparison. Repeat
with the other declared seeds and alternate arm order; motion adds
`--motion-offset 0.1` and requires separate phase summaries. The final combined
comparison uses separately frozen executables and omits `--guide-cache` to
exercise their actual defaults. Disable Metal API/shader validation and capture
instrumentation for timing, and retain the report's source/flags fingerprints.

## Exact guide-cache static qualification

The three-seed cache comparison completed all 36 runs (5,760 total frames,
including warmup). The table reports medians across seeds; p95 changes use the
median paired ratio rather than a ratio of pooled frames. Baseline and candidate
use the same frozen cache-capable executable at compiled revision `4365249`,
changing only `--guide-cache off|on`.

| Scene | Cache off median, ms | Cache on median, ms | Paired p95 reduction |
| --- | ---: | ---: | ---: |
| cornell | 73.932 | 70.620 | 4.38% |
| many-lights | 53.314 | 50.807 | 4.85% |
| glass | 107.312 | 90.082 | 16.23% |
| defocus | 46.588 | 33.271 | 28.58% |
| media | 144.560 | 129.247 | 10.75% |
| coated | 347.970 | 315.523 | 9.65% |

The declared defocus target exceeds the 10% p95 gate; every other scene/seed
remains below the 3% regression limit. All 36 final raw/reconstructed image pairs
are bit-identical, and per-frame sample totals, logical counters and history
decisions agree. This is a static execution comparison, not a new estimator or
denoiser-quality result. Motion qualification and the final combined stack are
reported separately.

## Guide-cache motion qualification

The initial motion study at `040b941` completed 30 runs, 4,800 total frames and
30 bit-identical final raw/reconstructed image pairs. All per-frame sample and
evaluation totals, logical reconstruction counters and history decisions agree.
Across every scene/seed, cache-on hits are 62/64 during warmup, 24/24 during the
first hold, 0/24 during translation, 23/24 during the final hold and 22/24 after
the cut. Every hit generates zero surface pixels and every miss generates
2,073,600. This verifies the expected work skipping and invalidation for these
trajectories; it does not establish moving reconstruction quality or per-frame
image identity.

Before execution, the motion safeguard required each scene/phase's median paired
p95 ratio to remain at most 1.03, without imposing a 10% gain during motion.
All 20 initial medians pass, but individual seeds straddle the threshold for
Cornell after the cut (1.0413 in seed 41002), glass translation (1.0317 in seed
41001), and coated translation (1.2662 in seed 41001). No cause is assigned to
those tails. The single predeclared confirmation uses seeds 41004–41006, all five
scenes, identical frame budgets and separate phase summaries. Both
confirmation-only and combined medians pass in every scene/phase. All 60 final
image pairs across the initial and confirmation studies are bit-identical, with
matching accounting and history decisions across 9,600 frames. The initial tail
regressions remain part of the record; no further confirmation is used.
The confirmation also contains an individual many-light translation ratio of
1.0351. It remains in the retained seed records; the predeclared decision uses
confirmation and combined medians, rather than requiring every individual tail
to pass.

The following values are median paired complete-pipeline p95 ratios, cache on /
off. Combined values use the ordinary median of all six seed ratios within each
scene/phase; phases are never pooled.

| Phase | Scene | Initial | Confirmation | Combined |
| --- | --- | ---: | ---: | ---: |
| hold-start | cornell | 0.9423 | 0.9563 | 0.9524 |
| hold-start | many-lights | 0.9436 | 0.9507 | 0.9471 |
| hold-start | glass | 0.8426 | 0.8390 | 0.8423 |
| hold-start | defocus | 0.7044 | 0.7138 | 0.7091 |
| hold-start | coated | 0.9052 | 0.8991 | 0.8998 |
| translate | cornell | 1.0017 | 0.9971 | 1.0009 |
| translate | many-lights | 0.9856 | 0.9861 | 0.9858 |
| translate | glass | 1.0069 | 0.9976 | 1.0020 |
| translate | defocus | 1.0113 | 0.9937 | 1.0032 |
| translate | coated | 1.0025 | 0.9995 | 1.0010 |
| hold-end | cornell | 0.9637 | 0.9584 | 0.9604 |
| hold-end | many-lights | 0.9493 | 0.9609 | 0.9512 |
| hold-end | glass | 0.8504 | 0.8426 | 0.8447 |
| hold-end | defocus | 0.6944 | 0.6966 | 0.6955 |
| hold-end | coated | 0.9080 | 0.9061 | 0.9071 |
| cut-start | cornell | 0.9773 | 0.9719 | 0.9746 |
| cut-start | many-lights | 0.9763 | 0.9805 | 0.9784 |
| cut-start | glass | 0.9791 | 0.9854 | 0.9823 |
| cut-start | defocus | 0.9489 | 0.9591 | 0.9577 |
| cut-start | coated | 0.9665 | 0.9910 | 0.9788 |

## Retained packet screening failure

The initial runtime-selected layout/event implementation (`e8acf6d`) passed
image controls but regressed the existing packed/one-event coated path. A
separate cross-source screen measured 315.31 ms before the packet change
(`55f0180`) versus 457.36 ms afterward: its paired p95 ratio is 1.459. The five
scalar fixtures have p95 ratios between 0.986 and 1.014. All 12 final raw and
reconstructed image pairs in that control remain bit-identical.

Two-event fusion reaches about 377 ms within the slower implementation, while
hot/cold splitting and split trace alone show no material gain. A gain against
that regressed control is insufficient for adoption. This experiment is
retained as rejected; compile-time policy specialization must be measured
against the pre-packet executable before any packet default can change.

## Native build-policy screen

All three optional `PreferFastIntersection` policies completed the fixed screen
(six scenes, seed 41001, eight warmup and 24 measured frames per arm; 36 runs,
1,152 frames). Every final raw/reconstructed image pair is bit-identical.
The declared many-light target produces these complete-pipeline p95 ratios:

| Policy | Candidate / default p95 | Retained AS bytes, both arms | Build wall time default / candidate, ms |
| --- | ---: | ---: | ---: |
| fast-blas | 0.9977 | 32,896 | 11.677 / 17.719 |
| fast-tlas | 1.0080 | 32,896 | 12.801 / 19.079 |
| fast-all | 1.0225 | 32,896 | 13.940 / 12.427 |

No policy approaches the 10% target, so no AS candidate advances to full
qualification or becomes a default. Single build timings are descriptive, not a
statistical build-time study. These fixtures use small structures; the result
does not establish the policy's value for large geometry. The opt-in controls and
actual allocation/build telemetry remain available for that separate study.

## Opaque-shadow qualification

The full comparison completed 36 runs and 5,760 total frames at compiled
revision `22487c7`, changing query versus eligible opaque Boolean traversal. All
36 final raw/reconstructed image pairs are bit-identical; sample totals, logical
reconstruction counters and history decisions agree on every reported frame.

| Scene | Query median, ms | Opaque-auto median, ms | Median paired p95 ratio | Individual seed p95 ratios |
| --- | ---: | ---: | ---: | --- |
| cornell | 70.185 | 67.014 | 0.9591 | 0.9591, 0.9731, 0.9467 |
| many-lights | 50.501 | 48.647 | 0.9540 | 0.9540, 0.9525, 0.9651 |
| glass | 89.516 | 85.391 | 0.9502 | 1.0750, 0.9485, 0.9502 |
| defocus | 33.286 | 34.291 | 0.9924 | 1.2661, 0.9738, 0.9924 |
| media | 129.011 | 129.212 | 0.9986 | 0.9279, 0.9986, 1.0040 |
| coated | 315.438 | 314.756 | 0.9980 | 0.9943, 0.9987, 0.9980 |

Cornell, the declared target, improves by only 4.09% in median paired p95 and
misses the 10% requirement in every seed. The first-seed defocus and glass tails
also regress; those observations remain in the table. The policy is rejected for
default promotion without using the optional confirmation budget: the target
result is clearly below the adoption requirement. Query remains the default,
with opaque-auto available explicitly. No cause is assigned to the variable
non-target tails from these timings alone.

## Reconstruction-counter qualification

The mechanical SIMD reduction comparison uses frozen `4365249` versus `f83065d`,
with the guide cache enabled in both arms. All 36 runs (5,760 total frames)
complete; all 36 final raw/reconstructed image pairs are bit-identical and all
per-frame logical counters, sample totals and history decisions agree.

| Scene | Baseline median, ms | SIMD median, ms | Median paired p95 ratio |
| --- | ---: | ---: | ---: |
| cornell | 70.519 | 70.166 | 0.9955 |
| many-lights | 50.445 | 50.368 | 1.0013 |
| glass | 89.753 | 89.426 | 1.0018 |
| defocus | 33.035 | 33.272 | 1.0025 |
| media | 128.968 | 128.848 | 1.0002 |
| coated | 315.387 | 315.097 | 1.0004 |

Individual seed p95 ratios range from 0.9891 to 1.0077. This establishes no
meaningful performance gain. The mechanical change is retained under its
predeclared exemption from the 10% structural threshold; no measured regression
approaches the 3% limit, and no confirmation is triggered.

## Retained specialization screen

The next implementation (`650b9f9`) moves policy selection into Metal function
constants and removes the nullable continuation-hit fallback. It passes the
expanded correctness controls, but its complete-pipeline packed/one-event
control still measures 352.09 ms versus 315.39 ms for the frozen pre-packet
renderer: paired p95 is 11.76% higher. All final images in the control and
alternative screens agree exactly (24 raw/reconstructed pairs, 768 frames).

| Alternative | Median, ms | p95, ms |
| --- | ---: | ---: |
| hot-cold-1 | 350.269 | 355.256 |
| hot-cold-2 | 327.072 | 330.356 |
| hot-cold-4 | 328.391 | 331.638 |
| packed-2 | 327.880 | 330.823 |
| packed-4 | 328.306 | 331.247 |
| split-trace-1 | 350.760 | 354.305 |

Every alternative remains slower than the pre-packet control, so this version
is also rejected for promotion. The next repair restores the original ordinary
function and packed/one-event kernel separately from experimental dispatch.
The hypothesis concerns changed code generation; no register-pressure cause is
established. The original workloads, image requirements and thresholds remain
unchanged.

## Original-path restoration

The restoration (`6f4a752`) preserves the original ordinary event and shade
kernel tokens, placing experimental policies in a separate entry. Its coated
control measures 314.913 ms versus 314.692 ms before the packet change;
the paired p95 ratio is 0.9942. This fixed screen repairs the observed
default regression, without establishing a speedup. All 24 final image pairs
across the control and six-policy screen remain bit-identical.

Every alternative in the shared experimental entry still regresses p95
(1.0366–1.1145 times the restored packed control). None is promoted. One final
bounded source comparison uses seven fixed shade entries with literal storage,
event-count and hit-source paths. It retains the same workloads and gates; no
register-pressure explanation is inferred from these timings alone.

## Fixed packet entries

The final packet implementation (`040b941`, with documentation-only review head
`f4f65d5`) expands one shared shade body into seven fixed entries. The ordinary
event body and packed/one-event shade tokens match the pre-packet source; literal
event counts and storage paths remove the experimental entry's policy branches.
The full prepared BSDF remains intact. This source structure does not establish
a register-pressure explanation for the measured differences.

The original-path control now measures 313.238 ms versus 314.002 ms before the
packet experiments, with a paired p95 ratio of 1.0028. The six-policy screen gives:

| Alternative | Median, ms | p95, ms | Paired p95 / packed-one |
| --- | ---: | ---: | ---: |
| packed-2 | 303.923 | 309.405 | 0.9739 |
| packed-4 | 304.085 | 309.185 | 0.9715 |
| hot-cold-1 | 352.254 | 356.308 | 1.1195 |
| hot-cold-2 | 327.440 | 330.269 | 1.0379 |
| hot-cold-4 | 327.153 | 331.338 | 1.0398 |
| split-trace-1 | 352.302 | 355.155 | 1.1147 |

All 24 final raw/reconstructed image pairs across the control and policy screen
are bit-identical, with matching accounting across 768 frames. Packed fusion's
2.6–2.9% p95 reduction remains below the 10% gate, outside the confirmation band.
No packet candidate advances to full qualification or default promotion. Packed
one-event execution remains the default; the opt-in policies retain the measured
negative results. No further packet source screens are part of this study.

Separating hot and cold state does not reduce total retained state. At capacity
65,536, the four packet-specific buffers across both resident frame slots request
the following lengths, including 16-byte sentinels for unused pools:

| Layout | Requested packet-buffer bytes, both slots |
| --- | ---: |
| packed, any event count | 161,480,800 (154 MiB + 96 B) |
| hot-cold, any event count | 161,480,768 (154 MiB + 64 B) |
| split-trace | 167,772,192 (160 MiB + 32 B) |

These values derive from the allocation branches and measured wire sizes, not
from process RSS or device-wide Metal allocation. They exclude queues, replay,
images, arguments, scene resources and the 32 bytes of unused scalar-state
sentinels. Allocation granularity and residency can add costs. Split trace adds
two 48-byte-per-slot hit pools; the original full prepared vertex is retained
in every layout. The guide cache reuses its existing eight surface textures
instead of allocating a second guide set.

## Integrated correctness checks

Clean source `54fa952` passes 14 focused tests in both Release and ASan/UBSan,
with Metal API validation and no skips. Each preset covers packet scattering,
wavefront frames/queues; raw, temporal, signed and guide reconstruction; the
temporal denoiser; opaque shadows, native intersection characterization and AS
build policies; benchmark CLI options, viewer options and camera reconstruction.
The three groups take 27.04 / 122.71 / 1.20 seconds in Release and
63.44 / 112.03 / 8.96 seconds with sanitizers. These are correctness-test durations.

A separate three-frame 64² Release smoke omits cache and packet controls. It
reports cache on, packed-one execution, native query traversal and default AS
builds, with cache hits `[false, false, true]`, complete sample totals and finite
FLOAT raw/reconstructed captures verified by an independent decoder. The source
fingerprint includes both shader-body include files and agrees with the compile
definition and embedded executable description. Frozen Release artifacts retain
their exact source manifest and binary/metallib hashes.

Earlier packet implementation `040b941` also passes its three scheduling checks
in Debug, Release and ASan/UBSan (15.47 / 18.30 / 17.30 seconds). Its 14 policy
controls cover 42 frames; three combined feature arms add nine frames, with exact
final raw/reconstructed image controls. Contract eight rejects the retained older
library. The initial overstrict refill capability-check failure remains recorded;
the corrected check requires the constants each selected entry actually uses.
Independent source and numerical-summary reviews report no remaining findings.
See the PR validation summaries for mandatory full Debug publication and hosted
CI results; the focused counts above remain tied to their measured source.
