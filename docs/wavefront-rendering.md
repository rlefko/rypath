# Wavefront frame execution

Live frame sessions default to `FrameExecution::wavefront`. The same session API
also offers `megakernel` for matched comparisons. Both execute the current PT
transport model and publish one complete raw XYZ estimate per pixel. The offline
sample API and BDPT remain separate. Native and compensated intersections are
independent choices; compare execution modes using the same intersection profile.

## Scheduling

A bounded pool holds up to 65,536 active paths. GPU queues carry pool indices;
finished paths publish their owner contribution and release their slot for the
next pixel. Camera randomness depends on the pixel and absolute sample index,
not its slot, queue order or command buffer. The primary texture footprint uses
the session's fixed sample budget.

Scalar paths have separate intersection and shading dispatches. Packet paths
trace and shade one event per dispatch and retain three rolling vertices for PT
connections and MIS. Direct-light visibility runs inside shading. This version
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

Hardware results and performance qualification are pending for this integration.
The retained [resident-frame baseline](frame-rendering.md) predates wavefront
execution and must not be presented as its measured performance.

The execution structure follows the motivation in
[Megakernels Considered Harmful](https://research.nvidia.com/sites/default/files/pubs/2013-07_Megakernels-Considered-Harmful/laine2013hpg_paper.pdf).
Our queue layout, packet staging and asynchronous command policy are adaptations;
published GPU speedups do not establish M4 Max results.
