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
complete. The final FLOAT capture pixels agree bit-for-bit in all six scene/seed
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

### Integrated qualification snapshot

A later comparison repeats the same 12-run protocol at clean source `0ef51ba`,
which also includes the Area packet-resource compiler compatibility fix. The
original baseline remains frozen at `c8b94b1`. All 288 measured and 48 warmup
frames complete; all six matched final capture pairs again have bit-identical
RGB pixels (37,324,800 components). Both reports and EXR metadata confirm scalar
Cornell and packet coating. This is separate evidence from the isolated storage
comparison above, not an attribution of every integrated timing difference to
storage alone.

| Scene | Build | Median range (ms) | Observed p95 range (ms) |
| --- | --- | ---: | ---: |
| Cornell scalar | Frozen baseline | 50.776–51.031 | 51.638–52.013 |
| Cornell scalar | Integrated compact state | 48.813–51.103 | 50.432–52.087 |
| Coated packet | Frozen baseline | 390.798–391.139 | 393.691–394.018 |
| Coated packet | Integrated compact state | 290.334–291.721 | 293.607–294.492 |

Paired packet medians fall 25.4–25.8%; scalar results are mixed. Packet process
peak RSS is 569.6–575.9 MiB for the baseline and 569.9–576.6 MiB for the candidate,
so no RSS reduction is established. These processes also perform final captures;
their high-water marks have a different scope from the capture-free pipeline
matrix. Results remain raw-only, descriptive measurements, not a quality pass.
Evidence is retained under
`.cache/validation/packet-storage-001/paired-release-003/`.
See [pipeline qualification](pipeline-qualification.md) for independent/Area
denoised timing, image quality and sustained measurements at the same snapshot.

The execution structure follows the motivation in
[Megakernels Considered Harmful](https://research.nvidia.com/sites/default/files/pubs/2013-07_Megakernels-Considered-Harmful/laine2013hpg_paper.pdf).
Our queue layout, packet staging and asynchronous command policy are adaptations;
published GPU speedups do not establish M4 Max results.

## Opt-in packet scheduling experiments

`FrameConfig.packet_events` and `--packet-events 1|2|4` select how many unchanged
packet events execute locally before publishing state. The default remains one.
An event includes one trace/shade attempt or transparent-boundary crossing;
termination stops the local loop immediately. The loop does not renumber random
streams, alter coating walks, reorder endpoint/direct contributions, or truncate
paths. Scalar transport and megakernel execution reject nondefault packet
controls instead of silently ignoring them.

`FrameConfig.packet_layout` and `--packet-layout packed|hot-cold|split-trace`
select storage/execution. `packed` retains the default full per-slot state.
`hot-cold` stores ray/sample/wavelength/throughput/radiance/progress state apart
from the complete frozen latest Vertex and assembles the original thread state
for shading. The Vertex retains all prepared scattering lanes and scene-table
pointers: represented directional PDFs still evaluate the prepared BSDF using
neighbor geometry, rather than substituting the sampled PDF.

`split-trace` uses the same separate pools and adds a per-slot checked hit buffer.
A separate trace dispatch reads only hot state and publishes the hit plus any
alpha/sample state updates. Shading consumes that hit through the original event
body. This mode requires one packet event per publication. Existing producer/
consumer barriers publish hit and hot-state writes before shading; refill fully
initializes hot and cold owner state before slot reuse. Only the selected large
pools are allocated; other argument bindings use small sentinels. The shader
handshake reports each allocation's size/alignment, and host per-buffer and total
working-set checks include the selected pools. Wavefront contract 8 rejects older
libraries and validates the current fixed-entry policy mapping.

Reports retain requested packet settings and effective settings (null when no
packet execution applies), and raw captures record both controls. These are
execution experiments, with no established speedup. `metal.wavefront_frame`
adds exact paired raw-XYZ controls for packet sky, diffuse, glass, coating and
null/invisible history, capacity one/seventeen and high absolute sample indices.
It also checks retained lifetimes, sample/evaluation accounting, nonfinite-output
failure and explicit unsupported-policy rejection.
Additional targeted controls use a stochastic glTF alpha sheet and adjacent
triangle/bilinear-patch receivers. Native intersections cover all seven policies
at capacity seventeen; compensated intersections cover each layout with one
event. Both use high absolute sample indices, the existing direct-sample sensor
oracle tolerance, and bitwise same-profile policy comparisons. These additional
alpha/mixed-geometry/profile controls pass in all three presets; the later
contract-8 record supplies current correctness evidence. The original bounded
record below predates these additions. The tests require actual GPU execution;
building them alone supplies no hardware equivalence evidence.

### Bounded packet-policy correctness evidence

On November 23, 2026, Debug source digest
`e7b53300244884b27a848d7e69cc82812f36167e1847eb9ce3b7bf89c1750d32`
on M4 Max / Darwin 25.6.0 / AppleClang 21.0 passes the expanded wavefront-frame
and queue tests with Metal API validation (123.91 s and 0.26 s). These are
correctness-test durations, not performance measurements. The shader reports
1,232-byte packed state, a 224-byte hot state, 1,008-byte retained Vertex, and
48-byte split hit record; alignments are 16/16/16/8 bytes. The new host explicitly
rejects the older contract-4 metallib.

A separate 64×64, three-frame smoke covers all seven valid layout/fusion policies
for both independent and Area-one replay, with temporal reconstruction: all 42
pipeline frames complete. Independent OpenEXR 3.4.4 / NumPy 2.4.5 decoding confirms
finite FLOAT RGB and correct requested/effective report settings and raw EXR
metadata. Every policy's final raw and reconstructed RGB values agree bit for bit
with its same-estimator packed/one-event control. These controls retain the
fixture's existing finite-layered-model diagnostic; scheduling adds no coating
cutoff. This bounded evidence establishes neither general bit identity nor a
speedup, and does not replace a matched Release performance/quality study.
Receipts, exact commands, per-policy images, decoder script, hashes and the ABI
probe are retained outside Git under `.cache/validation/packet-schedule-001/`.

### Contract-6 specialization experiment

The first runtime-policy experiment remains retained in history and frozen
artifacts. Its paired screen is separate from the specialization below; no
policy is promoted by changing the implementation mechanism.

Apple's [function-specialization guide](https://developer.apple.com/documentation/metal/using-function-specialization-to-build-pipeline-variants)
describes pipeline variants with function constants; its
[unspecialized function lookup](https://developer.apple.com/documentation/metal/mtllibrary/makefunction(name:))
provides function-constant metadata for capability checks. Our seven-policy
adaptation does not establish register occupancy or a speedup.

Session construction specializes wavefront-only Metal function constants 23
(layout) and 24 (events), retaining the scene's material, intersection and opaque
visibility constants. All seven supported policies share kernel source but get
separate specialized pipelines. The packed/one-event branch has a single call
to the original three-argument, non-inlined packet-step interface, with direct
packed state access and no runtime event loop or nullable hit source. The
numerical event body is force-inlined into the ordinary and pretraced step
functions; prepared scattering, random draws and contribution order are unchanged.
The split function receives a non-null hit reference and has no event-trace
fallback. Its ready flag is checked only against the original event eligibility;
complete, zero-throughput and invalid-ray states still reach their original
termination/failure handling without requiring a hit.

Wire contract 6 preserves the buffer layouts and rejects older libraries before
loading policy pipelines. The host verifies specialization constants on the
actual selected packet entry; effective report settings remain null until a
session builds successfully. Reset checks runtime config against compiled
policy constants. The queue test covers all 49 compiled/requested-policy pairs
and requires a mismatch to fail before issuing or completing owners. A matched
performance comparison against both the frozen pre-packet implementation and
the retained runtime-policy experiment remains separate; no speedup is established
by this implementation mechanism or the correctness checks below.

The existing packet-scattering GPU probe also exercises the production split-hit
guard through a private shared shader helper, without adding a renderer/host
test API. Its fixed outcome table includes eligible missing and
corrupt hit flags, a valid hit, complete/depth/zero-throughput exits, invalid ray
and throughput inputs, and stale/sentinel hit flags. This isolates the new guard
branches; the full frame controls separately validate original completion,
arithmetic failure and raw-image behavior.

#### Specialization correctness checks

On November 23, 2026, source digest
`2cdb69d00f1e06818117816833d168281da6ea5078f49d13967d92bf457114f8`
passes the packet-scattering, expanded frame and queue checks on M4 Max in Debug,
Release and ASan/UBSan with Metal API validation. The 14 ready-guard cases and
49 compiled/requested-policy pairs pass, as do the alpha, procedural and
compensated-profile image controls. The thread-reference pretraced wrapper
compiles on the validated Metal toolchain. Strict LLVM 18 portable syntax and
focused clang-tidy checks pass, along with CLI/motion/diagnostics tests.

The initial Debug frame attempt timed out at 300.04 s; a contemporaneous process
snapshot showed its Metal compiler service at 99.2% CPU while the test waited.
That failed attempt remains retained. The frame-test bound is now 900 s, and
fixture/policy progress is logged before pipeline creation. The retry passes in
49.27 s; this cache-dependent duration is not a renderer-performance measurement.
Release's three focused GPU checks pass in 28.93 s and ASan/UBSan's in 16.57 s.
No numerical comparison or failure check was relaxed.

All seven policies complete three 64×64 frames for each of independent and
Area-one replay, with temporal reconstruction (42 complete pipeline frames).
Independent OpenEXR/NumPy decoding finds every final raw and reconstructed image
bit-identical to its same-estimator packed/one-event control. An additional
nine-frame control combines split tracing, opaque-auto shadows, fast-all native
AS builds, cached guides and profiling on/off: final raw and reconstructed
images agree bit for bit with query/packed controls, and profiled stage samples
are complete and finite. The cache correctly misses twice while previous-camera
validity changes, then hits; an initial harness expectation of a frame-one hit
was corrected with its failed audit retained. The host explicitly rejects the
frozen contract-5 metallib, whose SHA-256 remains unchanged.

Evidence, exact commands, images, decode hashes and failed attempts are retained
outside Git under `.cache/validation/packet-specialization-001/`. The fresh
Release executable and metallib are frozen separately as
`core-speed-001/frozen-packet-specialized`; earlier frozen builds are unchanged.
These are bounded correctness controls, not occupancy or speedup measurements.

### Literal ordinary-path restoration

The runtime-policy and contract-6 specialization experiments remain retained in
Git and frozen artifacts; the lead's matched screens rejected them for an
introduced default-path regression despite exact image controls. The current
repair restores ordinary packed/one-event source structure before another
matched performance comparison. No policy, workload or acceptance bound changes.

The original `packet_pt_step` body is stored once in the private, intentionally
unguarded `packet_pt_event_body.inc`. Ordinary and pretraced noinline functions
include it with a compile-time hit-source selector. The ordinary expansion keeps
`f0d8494`'s exact signature, numerical body, intersector declaration/set-cull
placement and inline event trace statements. The split expansion substitutes only
hit acquisition and omits the event intersector; it has no event-trace fallback.
The template/always-inline wrapper experiment is removed.

`rypath_wavefront_shade_packet` again contains the original direct packed-state
load, single step call and packed-state publication. A separate experimental
entry handles the other six policies. The host selects the ordinary entry only
for packed/one-event and supplies its original scene constant set. Opaque-shadow
constant 22 is verified on the actual selected entry; policy constants 23/24 are
verified on the experimental entry and reset producer, with runtime policy
consistency still checked before owners are issued. Wire contract 7 rejects
older entry-routing libraries without changing resource sizes or state layouts.

The body include is both an explicit Metal rebuild dependency and an input to
the actual `BuildProvenance.cmake` source fingerprint (`shaders/*`, with no
extension filter). An independent full-source manifest, including the `.inc`,
agrees with CMake's embedded digest. Token receipts compare 2,733 ordinary-body
tokens, 32 signature tokens and 409 default-kernel tokens with `f0d8494`; all
agree. These checks establish source parity, not binary identity, register
occupancy or restored throughput. Build/GPU and paired image evidence for this
repair is recorded separately under `.cache/validation/packet-restoration-001/`.

At source digest
`653422fa55d2c91df9ad670d06af038e0c5b7640a353e69cb2e204f50dd61d72`,
packet-scattering, expanded wavefront-frame and queue checks pass under Metal API
validation in Debug (303.46 s), Release (28.27 s) and ASan/UBSan (17.14 s) on
M4 Max. Debug's cold frame check completes in 299.02 s under the retained 900 s
bound. These are test durations, not rendering-throughput measurements. Both
frozen contract-6 and contract-5 libraries are explicitly rejected, with their
file hashes unchanged.

The same 14 policy controls complete 42 pipeline frames, and the combined
split/opaque-auto/fast-all/cached-guide/profiling control completes nine more.
Independent decoding again finds final raw and reconstructed images bit-identical
to the corresponding packed/one-event controls; profiled stage samples remain
complete and finite. No attempt failed in this restoration validation. The prior
failed/rejected experiments stay in their own records. Clean Release artifacts
and source/file provenance are frozen separately as
`core-speed-001/frozen-packet-restored`; qualification against the original
frozen workload remains required before claiming the default regression fixed.

### Contract-8 fixed-entry experiment

The contract-7 restoration retained the original ordinary event and packed1 shade
body. Its measured default control matched the pre-packet control, while the six
shared-entry alternatives remained slower. Those results and frozen artifacts
remain retained; they do not establish a benefit for an experimental policy.

Contract 8 replaces the shared experimental shade entry with seven named entries.
A single `packet_shade_body.inc` expands literal layout, event-count, and hit-source
paths: packed policies access the packed pool directly, separated policies call
unconditional hot/cold accessors, and split trace calls only the pretraced event.
Every shade pipeline receives the original scene constants. Producer pipelines
retain constants 23/24 and immutable configuration checks; the host still verifies
opaque-shadow constant 22 on the actual selected shade entry. Contract 8 rejects
older libraries before loading this entry map. No policy defaults change.

Source expansion checks retain the original `f0d8494` ordinary event's 2,733 body
tokens and the packed1 kernel's 409 tokens. Both shared body includes participate
in shader dependencies and the actual source fingerprint. This checks source
structure and numerical ordering, not generated code, occupancy, or performance.
Fresh validation evidence is retained outside Git under
`.cache/validation/packet-fixed-001/`; performance qualification remains separate.

The three focused GPU tests (`packet_scattering`, `wavefront_frame`, and
`wavefront_queue`) pass in Debug, Release, and ASan/UBSan with Metal API validation.
Seven-policy independent and Area controls (42 complete frames) retain bit-exact
raw and reconstructed captures within each estimator. Three combined control
arms retain exact captures with opaque traversal, fast-all AS builds, guide reuse,
and stage profiling; profiled stages are complete and finite. The new host rejects
the frozen contract-7 library. Reflection records layout 23 on refill and both
23/24 on reset, initialize, and prepare-refill. An initial overly strict refill
capability check failed visibly; its log remains retained with the corrected runs.
