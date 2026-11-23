# Wavefront frame execution

Live frame sessions default to `FrameExecution::wavefront`. The same session API
also offers `megakernel` for matched comparisons. Both execute the current PT
transport model and publish one complete raw XYZ estimate per pixel. The offline
sample API and BDPT remain separate. Native and compensated intersections are
independent choices; compare execution modes using the same intersection profile.

## Scheduling

A bounded pool holds up to 65,536 active paths. GPU queues carry pool indices;
finished paths publish their owner contribution and release their slot for the
next pixel. A single GPU dispatch reserves each refill's contiguous pixel range;
lanes then initialize distinct owners without competing in a compare-exchange
loop. Camera randomness depends on the pixel and absolute sample index,
not its slot, queue order or command buffer. The primary texture footprint uses
the session's fixed sample budget.

Scalar paths have separate intersection and shading dispatches. Packet paths
trace and shade one event per dispatch. They retain the latest full vertex and
the preceding vertex's position/type; the current event stays local until its
endpoint and direct contributions consume that history. Null and invisible
boundaries preserve it. Direct-light visibility runs inside shading. This version
has no material sorting or separate shadow queue. Scalar homogeneous-medium
state, null-boundary progress, packet wavelength termination and eta-aware
roulette survive dispatch boundaries. Queue changes do not introduce a depth,
roughness or radiance cutoff.

Each command executes a configurable number of rounds, then its completion
handler checks GPU counters and submits another command if paths remain. This
quantum bounds command work; it is not a path termination rule. Barriers publish
queue and state writes between producer and consumer dispatches. Indirect
arguments keep empty queues safe. Overflow, duplicate owners, invalid sample
values and inconsistent counts fail the frame explicitly. Partial frames cannot
be captured as successful output.

Two frame slots retain their resources through GPU completion and handle
release. A retained frame survives destruction of its session. Resource sizing
uses a shader-reported state ABI and checked allocation budgets. Raw images stay
on the GPU; explicit capture waits for completion and performs a separate copy.

## Reproducible comparisons

The benchmark accepts `--execution wavefront|megakernel`,
`--intersections native|compensated`, `--capacity 1..65536`, `--quantum 1..1024`
and `--group-width 1..256`. Device pipeline limits are also checked. Reports and
EXR metadata record these controls and completed command counts. Capacity,
quantum and group width describe wavefront scheduling and do not alter the
megakernel control.

CPU wall time spans submission through final completion, including host callback
gaps. GPU time is the sum of valid command-buffer timestamp intervals; it omits
those gaps. Neither metric includes optional image capture. Use CPU wall time
for complete serial frame throughput. Compare raw output and time together;
more dispatches or higher ray throughput alone do not establish an improvement.

## Validation

`metal.wavefront_frame` compares matched absolute samples against the megakernel
and independent host sensor reduction. Cases cover scalar and packet diffuse,
glass, coating, geometry-free media and null-boundary chains, including sample
indices above 2^32. Capacity-one and partial-group schedules force slot reuse
and repeated commands. The tests also exercise retained-frame lifetime, invalid
configuration and visible arithmetic failure.

`metal.wavefront_queue` injects invalid counts and a sticky overflow into the
actual queue-advance kernel. It checks visible failure, unchanged completion
counts and untouched guard storage. These checks supplement ordinary queue
execution; they are not a throughput measurement.

Focused Release and ASan/UBSan frame and queue checks passed with Metal API
validation after the range allocator change. The queue probe also checks full,
partial, exhausted and zero-free reservations, extra empty rounds, invalid
accounting and clearing the published range at the next dispatch boundary.

## M4 Max measurements

The initial November 23, 2026 resident comparison used the 40-core M4 Max, macOS 26.6
and Xcode 26.6, native 1920×1080, one fresh sample per pixel, native intersections,
three seeds and four warmup plus 32 measured frames per run. Both modes used the
same source, default capacity 65,536, quantum eight and group width 32. Other
GPU tests and CPU compilation were paused. All 432 frames completed with valid
GPU timestamps. CPU wall time includes completion-handler gaps.

| Scene | Execution | Median range (ms) | p95 range (ms) |
| --- | --- | ---: | ---: |
| Cornell | Megakernel | 93.63–94.05 | 96.19–97.16 |
| Cornell | Wavefront | 49.40–50.33 | 50.82–51.50 |
| Coated packet | Megakernel | 211.84–214.64 | 216.09–216.97 |
| Coated packet | Wavefront | 389.00–391.20 | 392.89–394.87 |

Wavefront improves Cornell by about 1.88× but makes the coated packet case about
1.83× slower. Both miss the complete-pipeline target: these measurements contain
no reuse, mutation, guide generation or denoising, and Cornell's p95 exceeds
50 ms even here. Packet state traffic and shading remain tuning work. The
retained earlier [native megakernel measurements](frame-rendering.md) are faster
than this build's megakernel controls and remain a separate regression reference.

### Compact packet state

The subsequent `826fccc` implementation reduces shader-reported packet state
from 3,232 to 1,232 bytes, or 202 to 77 MiB per 65,536-slot pool. Wavefront wire
contract 4 rejects a mismatched library. This changes execution storage, not
random draws, scattering, roulette, density arithmetic or accumulation order.
The current event is evaluated before rolling history forward.

Four focused wavefront/queue/Area/MCMC checks pass in each of Debug, Release and
ASan/UBSan with Metal API validation (12 checks total). Null and invisible
boundaries at depths 1/2/4, capacity 1/17, quantum one and indices above 2^32
exercise the retained-history dependencies and visible overflow behavior.

A separate isolated Release comparison against the frozen pre-compaction build
uses native 1080p, seeds 31001–31003, four warmup and 24 measured frames, default
capacity/quantum/group width, and independent raw rendering. All 336 frames
complete. The final FLOAT captures agree bit-for-bit in all six scene/seed
pairs (37,324,800 compared RGB components). This supports unchanged output for
these controls; it does not establish universal bit identity.

| Scene | State | Median range (ms) | Observed p95 range (ms) |
| --- | --- | ---: | ---: |
| Cornell scalar | Original | 49.83–50.39 | 50.73–51.33 |
| Cornell scalar | Compact | 50.42–50.59 | 51.34–51.64 |
| Coated packet | Original | 386.10–389.56 | 390.98–392.22 |
| Coated packet | Compact | 287.42–289.57 | 290.90–291.54 |

Packet median frame time falls 25.1–26.2%; Cornell is 0.05–1.36% slower in this
small repeated study. The packet result still exceeds both the earlier
megakernel control and the full-pipeline frame-time target. Process peak RSS
does not decrease: coated runs measure 552–576 MiB original and 570–580 MiB
compact. RSS is not total Metal allocation; the state-pool reduction is a
separate compiler-layout measurement. Observed p95 values describe these runs,
not a population guarantee or denoised performance.

Evidence is retained in `.cache/validation/packet-storage-001/paired-release-002`.
The preceding `paired-release-001` study used a generated coating scene that
selects scalar transport; it remains a scalar control and is excluded from the
packet speedup. The corrected comparison asserts the effective packet/scalar
backend from both the report and captured EXR metadata.

An independent OpenEXR 3.4.4 / NumPy 2.4.5 decoder checked all four FLOAT captures,
dimensions, finite values and sample/profile/execution metadata. The matched
seed-one, sample-35 RGB values are exactly equal between modes for both scenes
(6,220,800 values per scene). This verifies those sample controls, not convergence
or equal-quality rendering time.

The retained measurement source is `0e1010414ea2781ff9eb5a86cc1286eae8b29539`;
manifest SHA-256 is
`8c610dbafe9f13787d1cff3b3f1d8652d517424878fa9fe44fcd8f42455cf592`.
The executable and metallib are retained with the reports and captures. An earlier
incomplete study is retained separately: its per-lane CAS allocator measured
3.090 s for Cornell wavefront versus 93.99 ms for the matched megakernel. The run
was stopped for investigation; replacing contended allocation with one range
reservation produced the measurements above.

The execution structure follows the motivation in
[Megakernels Considered Harmful](https://research.nvidia.com/sites/default/files/pubs/2013-07_Megakernels-Considered-Harmful/laine2013hpg_paper.pdf).
Our queue layout, packet staging and asynchronous command policy are adaptations;
published GPU speedups do not establish M4 Max results.
