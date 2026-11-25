# Resident frame baseline

`rypath-frame-benchmark` measures a fixed camera scene with one new independent
camera sample per pixel per frame. Scene translation, native acceleration
structures, shading resources and pipelines are prepared once. The initial
control submits one frame and waits before submitting the next. The session API
has two retained frame slots; this command does not measure overlapping work.

```sh
rypath-frame-benchmark --demo --width 1920 --height 1080 \
  --warmup 8 --frames 120 --output /tmp/frame-baseline.json
rypath-frame-benchmark --scene /path/to/scene.pbrt \
  --width 1920 --height 1080 --warmup 8 --frames 120 \
  --seed 1 --material-seed 0 --output /tmp/scene-frame-baseline.json \
  --capture /tmp/scene-last-frame.exr --preview /tmp/scene-last-frame.png
```

All outputs and their `.pending` siblings must be fresh, distinct and separate
from the PBRT entry point and its inventoried input dependencies. An existing
file or symlink is rejected. Preflight runs before output reservation so asset
aliases can be checked. Failures after safe report reservation retain a failure
JSON with completed counts. A failed preflight retains its diagnostics and does
not attempt rendering. Exit 0 requires all warmup and measured frames to finish;
exit 2 means Metal/device/ray tracing is unavailable; other failures exit 1.

The defaults are 1920×1080, 120 measured frames, eight warmup frames, seed 1 and
material seed 0. `--scene` and `--demo` are mutually exclusive; `--output` is
required. The benchmark explicitly overrides authored resolution, sampler and
sample budget, stochastic seeds, and finite film clamping. Authored BDPT becomes
path tracing; authored volumetric path tracing retains volume transport. The
authored packet wavelength policy selected by BDPT or a finite film clamp is
retained for vacuum paths up to depth 64. Active media or greater depths
retain scalar transport with an explicit provenance note. Other unsupported
rendering semantics retain translation diagnostics. The report
records overrides and translation provenance. This is a live frame policy,
not an offline PBRT equivalence claim.

The independent sampler consumes distinct absolute sample indices: warmup uses
`[0, warmup)`, followed by measured indices `[warmup, warmup + frames)`. Texture
footprints always use a fixed one sample budget. Each raw frame replaces the
previous frame; frames are not progressively accumulated or denoised. Optional
capture exports the last successful frame, even when a later frame fails, as
unexposed FLOAT linear RGB EXR through the existing XYZ film conversion. Its
metadata identifies sample index, epoch, seeds, footprint budget, integrator,
spectral mode, source identity and build. PNG is a separate display preview
with the authored scene exposure. Export and readback are outside per-frame timings.

JSON schema 1 reports startup and warmup durations separately, every attempted
frame's sample index, completion/count/error, CPU submit-and-wait wall duration,
and actual command-buffer GPU duration. A missing or invalid GPU timestamp is
`null`, never substituted with CPU wall time. Median and p95 use nearest rank
over completed measured frames only; GPU summaries include only valid GPU
intervals and report missing timestamps separately. Startup includes preflight,
translation, reservation and resident session preparation. CPU and GPU durations
overlap and must not be added. Build, OS, device, scene dependencies and effective
settings identify the observation.

The native `FrameSession` API accepts complete camera snapshots without rebuilding
the scene. Translating a camera rebases texture graph and alpha definition
coordinates into completion-retained, per-slot buffers; `camera_upload_bytes`
reports that cost, and a repeated origin reuses its slot's cached mappings.
Callers provide matching texture camera transforms and explicitly identify the
camera medium after a boundary crossing. Non-vacuum camera overrides require
scalar PBRT volume transport. Failed submissions leave the session usable.

Live sessions default to `IntersectionProfile::native` in `FrameConfig`.
The benchmark exposes `--intersections native|compensated`, defaulting to
`native`; select `compensated` explicitly for a comparison baseline. This option
is immutable for a session. The existing offline renderer builders continue to
default to compensated intersections.

Native mode uses authoritative hardware triangle distances and barycentrics
while retaining alpha callbacks and procedural patch/analytic composition.
Near-edge candidate coverage, rounded distances and endpoint inclusion can differ
from compensated mode; native mode does not recover misses with a compensated
query. Both profiles retain unclamped raw samples and the same sampling policy.
See the [intersection profile and measured witnesses](geometry.md#explicit-native-triangle-profile).
Changing profiles requires a new session and separate sample history.

The selected profile is recorded as `settings.intersection_profile` in JSON and
`rypath.frame.intersections` in EXR metadata. Canonical `source_identity`
continues to identify the owned scene and sampling settings; it does not encode
this numerical execution choice. Compare or group results using both source
identity and intersection profile. Frame validation compares each profile's GPU
image against the direct sample kernel built with that same profile, including
empty geometry, demo geometry, scalar and packet paths; it does not require
native and compensated images to agree at numerical edge cases.

`--shadow-traversal query|opaque-auto` selects Boolean shadow traversal, with
`query` as the default. Eligible native triangle scenes can opt into the
[opaque intersector specialization](geometry.md#opt-in-opaque-boolean-shadows).
The immutable session build result reports requested/effective mode, eligibility
and its reason. JSON records these in `settings.shadow_traversal_requested`,
`shadow_traversal_effective`, `opaque_shadow_eligible`,
`opaque_shadow_eligibility_reason` and `shadow_traversal_fallback_reason`.
Before a successful build, effective mode and eligibility are `null`.
`shadow_traversal_frames` counts completed measured benchmark frames by effective
mode, excluding warmup. These are frame counts; there is no per-ray instrumentation.
A fallback keeps query traversal without changing the intersection profile.

Surface-guide reuse defaults on in `ReconstructionConfig`. The benchmark resolves
its default after parsing: temporal and MetalFX modes enable it, while raw-RGB
and no reconstruction leave it off. `--guide-cache on|off` overrides that default
regardless of option order; explicit on requires temporal or MetalFX. JSON
`settings.guide_cache` records the resolved setting. Fresh color conversion and
filtering still execute every frame, and raw sampling is unchanged. See
[guide reuse](reconstruction.md#exact-surface-guide-reuse) for invalidation and
[qualification](core-performance.md) for the static/motion evidence and limits.

## Initial M4 Max evidence

On 2026-11-22, implementation `1c1d7bb` passed the frame-session tests in Release
and ASan/UBSan with Metal API validation. The checks cover independent host sensor
reduction, scalar and packet tiles, dielectric geometry, translated world/alpha
textures, fixed footprints beyond sample index 2³², camera media, retained-frame
lifetime, capacity rejection and nonfinite-output failure. The portable availability,
CLI and statistics checks pass. Focused LLVM 18 clang-tidy and the native Clang
analyzer pass. Full publication checks are reported on the PR.

The resident **compensated-intersection megakernel** baseline uses an M4 Max
(40 GPU cores, 128 GB), macOS 26.6 and Xcode 26.6. Each scene has three independent
seeds, four warmup frames and 32 measured frames per seed at native 1920×1080,
one new sample per pixel, with API validation off. Scene order alternates. All
216 frames complete with exact pixel/sample accounting and valid GPU timestamps.
Captures occur after timing. The table gives ranges across the three runs.

| Scene and transport | CPU submit/wait median | CPU submit/wait p95 |
| --- | ---: | ---: |
| Cornell adapted, scalar PT, depth 65 | 1156.3–1164.7 ms | 1181.3–1191.1 ms |
| Coated path fixture, packet PT, depth 5 | 228.0–229.2 ms | 231.8–232.3 ms |

These are small-scene baseline measurements, with no reuse or reconstruction.
They do **not** meet the interactive target or establish image-quality equivalence.
Native triangle and wavefront changes must be measured against this retained
baseline rather than assuming published speedups apply.

The measured Release source digest is
`00eb5d22587441c92b1b11431d108d0e3893db2286a6129fc0de69072b2970d8`.
Local JSON reports, command manifests, raw FLOAT EXRs, PNG previews and frozen
executables are retained under `.cache/validation/resident-frames-001/`.

### Native traversal comparison

Revision `2ff4753` was measured on the same 40-core M4 Max, macOS 26.6 and Xcode
26.6 in Release, without concurrent GPU workloads. Each profile uses three seeds,
four warmup frames and 32 measured frames per seed at 1920×1080 and 1 SPP. Profile
and scene order alternate. All 432 submitted frames completed with valid GPU
timestamps. This is the resident megakernel; wavefront and reconstruction are
not part of this comparison.

| Scene | Compensated median frame time | Native median frame time | Native p95 range |
| --- | ---: | ---: | ---: |
| Cornell, scalar | 1156.2–1161.9 ms | 86.1–86.6 ms | 88.1–89.2 ms |
| Coated plane, packet | 228.5–229.5 ms | 196.6–199.4 ms | 201.1–201.4 ms |

The ratios of the three-run median frame times are **13.43×** for Cornell and
**1.157×** for the coated plane. These are CPU submission-to-completion times;
they exclude startup and explicit capture. Neither scene meets the 50 ms target.
The table establishes scene-specific execution gains, not time to equal converged
quality or the performance of the complete proposed pipeline.

Independent OpenEXR 3.4.4 decoding checks all four captured FLOAT RGB images for
full dimensions, finite values, profile metadata and matched seed/sample identity.
At seed 1, sample 35, the coated-plane RGB values agree exactly. Cornell's RGB RMS
difference is `3.3774e-7` and maximum absolute difference is `2.8634e-4`; these are
paired single-sample controls, not a general equivalence guarantee. The documented
native edge witnesses remain intentional differences between numerical profiles.

Receipts are retained outside Git under
`.cache/validation/native-frames-001/`: commands and per-frame timings in
`baseline/manifest.json`, independent decoding in `image-audit.json`, and source,
executable and metallib identities. Manifest SHA-256:
`02a8c7601781d80b1ec62f7c35c2aa55f1cf8164b3f256f512ffbe6889c6b746`.
An earlier capture attempt completed its frames but failed on an overlong EXR
attribute name; its report is retained separately and excluded from this table.
The corrected capture uses `rypath.frame.intersections`.

## Opt-in GPU diagnostics

`--profile-stages` enables timestamp counters around the existing compute
dispatches. It is off by default: no counter allocation, clock sampling or
resolution occurs unless requested. Command and encoder labels remain available
for Xcode capture. This diagnostic mode adds sampling barriers and host work;
its timings must not be used as an uninstrumented performance baseline.

Frame and reconstruction JSON include `gpu_stages`: requested/availability status,
completeness, dispatch counts and seconds grouped by stage. Wavefront exposes
reset/initialize, trace, shade, prepare-refill, refill and advance; Area exposes
its prepare/merge/mutation/temporal/spatial/finalize dispatches. Packet transport
traces inside shade. Reconstruction exposes reset, surface guides, color,
temporal accumulation, each spatial dispatch (aggregated), and finish. Opaque
MetalFX dispatches explicitly make reconstruction stage timing unavailable.
Megakernel timing separates initialize, render and resolve dispatches.
Raw RGB's output blit is outside the compute-dispatch diagnostic scope.

The ordinary timestamp counter set is required. Devices supporting dispatch-boundary
sampling keep the original encoders and insert counter samples. Devices supporting
only stage-boundary sampling use one serial compute encoder per logical dispatch
in profiling mode, with start/end samples attached through `MTLComputePassDescriptor`.
This fallback reports `encoder-stage-boundary` intervals and structural overhead:
the encoder boundaries and replayed resource declarations/bindings change the
execution schedule. Results describe this instrumented execution, not the production
schedule. The default unprofiled path retains its original encoder structure.
Interdispatch memory barriers remain before encoder completion; tracked resources
retain ordering within the same command buffer. Apple's
[stage-boundary capability](https://developer.apple.com/documentation/metal/mtlcountersamplingpoint/atstageboundary)
and [compute attachment indices](https://developer.apple.com/documentation/metal/mtlcomputepasssamplebufferattachmentdescriptor/startofencodersampleindex)
define this sampling contract. Unsupported counters, resolve errors, invalid clock
calibration, sentinel/regressing timestamps and command failures report explicit
status and null durations; subsequent dispatches cannot repair an incomplete
aggregate. Each command has at most 4,096 timestamp samples and each logical
frame/consumer has at most 64 aggregated stage records. A cap only invalidates
diagnostics; rendering continues. Counter buffers live through completion.
Timestamp seconds use two paired CPU/GPU clock samples and Apple's
[GPU-to-CPU time conversion](https://developer.apple.com/documentation/metal/converting-gpu-timestamps-into-cpu-time).
Stage intervals may overlap other work and are never summed as complete frame
GPU time. The existing command-buffer and CPU wall metrics retain their scopes.

For a bounded programmatic Xcode trace, launch with `MTL_CAPTURE_ENABLED=1` and
`--capture-frame N --capture-path /fresh/path/frame.gputrace`. N is a zero-based
absolute sample index including warmup and must be below warmup + frames. Capture
starts immediately before that frame's raw submission and stops after its
reconstruction completes, including any first-frame reconstruction setup. The
path must be fresh and distinct from all inputs/outputs. Unsupported capture or
start failures fail explicitly. Scope cleanup stops capture on failures and
exceptions; a partial failed trace is retained for diagnosis. Captures perturb
execution, and no performance claim should use a captured frame. The JSON records
the selected frame/path and profiling setting. Capture support and trace contents
require a separate hardware smoke check; portable tests do not establish them.

### Initial stage-diagnostics validation

On November 23, 2026, an M4 Max reports stage-boundary sampling support and no
dispatch-boundary support. Its timestamp sample buffer accepts at most 32 KiB:
the original 16,384-sample allocation failed with `MTLCounterErrorDomain` code 1
and an explicit 8–32,768-byte allowed range. The diagnostic buffer now uses at
most 4,096 timestamp samples (32 KiB); allocation failures retain Metal's error
domain, code and description.

Debug source digest
`fcffbf96c9cbf86ffa868cd4104c846c07366aa0ea9304b7d83d8e4044118452`
passes bounded 64×64, three-frame controls on the adapted Cornell fixture under
Metal API validation. Independent wavefront, Area replay with one mutation, and
megakernel execution all report complete finite encoder-stage timings, including
separate temporal reconstruction. Each arm has a matched unprofiled control;
all six raw/reconstructed FLOAT EXR file pairs are byte-identical. These 18
pipeline frames validate this fixture's scheduling/binding preservation, not
general equivalence or production performance. A separate 4×4, quantum-1024
frame completes rendering while explicitly reporting the counter-sample cap.
Fresh manifests, reports, logs, output hashes and allocation probe results are
retained outside Git under `.cache/validation/stage-diagnostics-001/`, with
successful runs named `counterfix-*`.
Earlier failed diagnostics reports remain separate from these successful checks.

### Repeatable camera-motion timing

`--motion-offset WORLD_X` adds a timing-only camera protocol: warmup holds the
authored pose, then four equal measured intervals hold that pose, translate along
world X, hold the translated pose, and cut back. The cut increments the frame
epoch. Measured frames must number at least eight and be divisible by four; the
offset must be finite, nonzero and representable by the active camera. Reports
record each frame's phase, offset, epoch and effective camera. The original
source identity still identifies the authored scene; the recorded camera
overrides are part of the workload identity.

Lens, camera medium and texture-camera matrices are preserved. Scenes with
different inside/outside medium interfaces are rejected because this driver
cannot classify camera crossings. This schedule exercises changing guides,
stationary reuse and cuts; moving-scene timings remain separate from static
timings and do not establish motion reconstruction quality. Captured EXRs record
the phase and offset of their actual captured sample.
