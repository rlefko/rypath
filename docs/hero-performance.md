# Hero pipeline performance

This follow-up preserves native 1920×1080, one fresh path per pixel, four-wavelength
hero sampling, one corrected mutation, and separate reconstruction. It targets
complete-frame speed and time to matched image quality for Cornell, Prism, and
Pavilion day/night, including camera movement. The first target is a 10× speedup;
the existing 20–30 FPS objective remains open.

## Measured starting point

The frozen spectral-color runtime (`2fa32662`, source fingerprint
`0b30faca98ce226238f454eb598f20ae53e9e2c0f4e61bff410faa8b65748763`) is the
production implementation merged through PR #73. Three measured frames following
two warmup frames give these diagnostic complete-pipeline rates with signed
temporal reconstruction, visible hero wavelengths, spectral-refresh mutation,
and reciprocal16 reuse. These short stationary controls localize cost; they do
not qualify sustained or moving-camera performance.

| Scene | Independent hero PT, FPS | Area + one mutation, FPS |
| --- | ---: | ---: |
| Cornell | 2.714 | 0.307 |
| Prism | 2.366 | 0.301 |
| Pavilion day | 1.191 | 0.144 |
| Pavilion night | 1.143 | 0.137 |

Warm Area-one performs eight full evaluations per pixel. Fused wavefront shading
accounts for 94–97% of instrumented raw GPU command time in Cornell/Prism.
Separate trace/shade execution is slower; its final raw FLOAT pixels agree exactly.
Cornell event fusion, larger groups and larger quanta also regress in the short
screen; a smaller pool is essentially unchanged. An isolated strict-FP `-O2`
compile passes the material-context check but exceeds the unchanged 180-second
hero-render deadline. It is not a qualified replacement for `-O1`.

These 22 diagnostic runs contain 94 native pipeline frames. The machine is a
40-core M4 Max with 128 GiB, macOS 26.6 and Xcode 26.6, on AC power without a
reported thermal warning. Performance controls disable API/shader validation,
capture and stage instrumentation. Startup and post-loop export are separate.
Artifacts remain outside Git under `.cache/validation/hero-performance-diagnosis-001/`.

## Hardware counter capture

A native 1080p Cornell independent-hero capture from the profiling implementation
was replayed with Xcode 26.6 on the same M4 Max. The shader table reports 192
allocated registers and 8.91 KiB of spilled storage for
`rypath_wavefront_shade_packet`; refill uses 168 registers and spills 528 bytes.
A representative bulk encoder reports a 75.18% MMU limiter, 42.85% last-level
cache limiter, and 2.86% ALU utilization. These are Xcode metrics, not additive
fractions of elapsed time. They support investigating large live state and spill
traffic before treating arithmetic throughput as the hardware limit.

The capture contains independent PT, not the eight-evaluation Area frame. Replay
profiling perturbs execution, so its duration is not a production FPS result.
The trace and transcribed counters are retained under
`.cache/validation/hero-performance-implementation-001/profiling/`.

## Path-work diagnostics

`--profile-stages` additionally reports `path_work` for wavefront frames. Each
row identifies an independent/fresh, temporal-forward, temporal-reverse, mutation,
spatial, or final-publication pass. Mutation/spatial indices identify separate
passes. Rows retain completed evaluations, submitted commands and encoded rounds,
command-boundary active-path observations, host encoding time, GPU command time,
and gaps between consecutive frame commands. The pool byte count covers one
frame slot's path-state buffers, not total renderer memory.

These are command-level measurements. A replay command can also merge the prior
pass or initialize the next one. Encoded rounds include empty tails; boundary
queue observations are neither bounce counts nor hardware occupancy. Gaps can
include other GPU work and CPU scheduling; they do not identify a specific cause.
Timestamp failures make timing unavailable without inventing zero-duration work.
Failed frames invalidate the diagnostic result. Existing per-dispatch timestamps
retain their separate scope and encoder-splitting overhead.

Packed one-event packet/hero execution also reports exact
`packet_step_invocations` per pass. This counts calls, including initialized-complete
paths and transparent-boundary crossings, rather than intersections or bounces.
The refill control accumulates consumed queue slots in a checked two-word counter;
noncompacted analytic null completions are subtracted because they bypass the
event. Compacted nulls never enter that queue. Exact temporal-cache hits execute
no event, and the final-publication pass reports zero calls. Scalar, separated
and multiple-event execution report unsupported with JSON `null`, never a
fabricated zero. Counter specialization is limited to reset/refill programs;
the shade program has no added per-path atomic operation. Profiling still has
structural overhead, so these counts do not qualify uninstrumented FPS.

The [Enhanced regression investigation](enhanced-performance.md) uses these
counts to test whether more packet-step calls explain the slowdown. A call can
contain multiple ray queries and other work; the count does not measure them.

## Ordered follow-up

1. Bind reproducible baseline/candidate runs and expose replay costs.
2. Validate compact hero history, exact scaled likelihood arithmetic, and shared
   wavelength-independent texture preparation as independent execution changes.
3. Cache temporal evaluations only when the complete physical context is equal;
   report actual and cached evaluations separately.
4. Derive and validate contribution-path reconnection under GRIS, with explicit
   spectral, delta, coating, medium, and corrected-mutation contracts.

Do not promote a candidate on throughput alone. Exact changes retain numerical
controls; estimator changes require independent-seed raw/reconstructed quality,
mean/tail/chroma and motion safeguards. Native AS traversal remains the default.

Primary guidance: [Apple GPU profiling](https://developer.apple.com/documentation/xcode/optimizing-gpu-performance),
[Apple shader resource utilization](https://developer.apple.com/videos/play/tech-talks/111373/),
[GRIS](https://graphics.cs.utah.edu/research/projects/gris/),
[corrected MCMC mutations](https://research.nvidia.com/publication/2024-07_decorrelating-restir-samplers-mcmc-mutations),
and [ReSTIR PT Enhanced](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/).
Published speedups are not measurements of this implementation.

## Paired qualification runner

`scripts/run_hero_performance.py` accepts explicit baseline/candidate executables
and the four scene paths. Defaults are eight warmup frames, 96 measured frames,
three seeds, and both static and moving-camera modes. Supply a nonzero
`--motion-offset scene=value` for each scene; motion holds the initial pose,
translates, holds the translated pose, then cuts back. The caller must keep the
GPU otherwise idle. The runner alternates paired execution order, binds binary
and metallib hashes, validates effective settings and per-frame accounting, and
retains each report, log, final raw/reconstructed EXR, and failure. Captures occur
after frame timing. It reports each scene/mode separately without a cross-scene
aggregate. `--frames`, `--warmup`, `--seeds`, `--modes static`, and
`--skip-captures` allow explicitly bounded screening; a short screen does not
replace qualification.

The per-arm `--baseline-area-contract` and `--candidate-area-contract` options
accept only `6` or `7` and default to `7`. For a pre-cache baseline, pass
`--baseline-area-contract 6 --candidate-area-contract 7`. The manifest and paired
results record these expectations and identify temporal evaluation caching as an
allowed execution-only difference; transport, MIS, mutation and reconstruction
settings stay matched. Contract 7 reports must supply boolean cache permission
and use flags, with exactly two cached evaluations per pixel on a permitted hit
with history. Actual plus cached evaluations must equal the logical count.
Contract 6 accepts the prior reports with absent cache fields and zero cached
work. Choosing a contract changes report validation, not the renderer CLI.

On the profiling revision, eight API-validated 64×48 runs (plain PT and Area with
zero, one, or four mutations, two frames each, baseline/candidate) retain exact
raw FLOAT pixel bits. Per-pass command and completed-evaluation sums agree with
frame totals, as do valid command GPU durations. The maximum four-mutation warm
frame has all 12 expected pass rows. CPU diagnostics and the portable runner's
10 tests pass, including seed-dependent identities, failure retention, and
owned-child cancellation. These checks validate instrumentation, not a speedup.
